# Как правильно и эффективно использовать систему {{product-name}}

В данном разделе собраны рекомендации по использованию системы {{product-name}} для сценариев массивно-параллельной обработки данных.

## Слишком мелкие чанки 

{% note info "Примечание" %}

Ситуация с мелкими чанками релевантна для статических таблиц. Сценарии работы с динамическими таблицами предполагают мелкогранулярные чтения для снижения времени ответа на запрос, поэтому небольшие чанки в случае динамических таблиц допустимы.

{% endnote %}

Маленьким можно считать [чанк](../../../user-guide/storage/chunks.md) размером менее 100 Mб. Чтобы чанков в системе не становилось слишком много, стоит стремиться к тому, чтобы средний размер чанка был не менее 512 Мб.

Мелкие чанки затрудняют работу кластера по нескольким причинам:

* Мелкие чанки создают дополнительную нагрузку на мастер-серверы. Чем больше чанков, тем больше памяти мастер-серверов требуется для хранения мета-информации о чанках, и тем медленней они работают: медленнее пишут snapshot, дольше восстанавливаются из него в случае проблем.
* Большое число мелких чанков приводит к медленному чтению данных. Если, например, 100 Мб данных расположены в миллионе чанков по одной записи в каждом, чтобы прочитать все данные, необходимо сделать миллион операций `seek` по диску, что очень медленно, даже если выполнять их параллельно.

В результате выполнения операций или мелкогранулярной записи данных могут возникать таблицы с большим количеством мелких чанков. Чтобы бороться с подобной проблемой чанки таких таблиц необходимо укрупнять. Укрупнение чанков можно выполнить вызвав команду:

```bash
yt merge --src //your/table --dst //your/table --spec '{combine_chunks=true;mode=<mode>}'
```

Следует установить значение атрибута `<mode>` в `sorted`, для [сортированной таблицы](../../../user-guide/storage/static-tables.md#sorted_tables) и `ordered`, для [несортированной](../../../user-guide/storage/static-tables.md#unsorted_tables). Подробнее о типах таблиц в разделе [Статические таблицы](../../../user-guide/storage/static-tables.md).

В обоих случаях merge сохранит порядок данных. Но в случае `sorted` он также сохранит сортированность таблицы с точки зрения системы: таблица останется `sorted` и сохранит все соответствующие атрибуты).

Если вы используете [Python-библиотеку](../../../api/python/userdoc.md), можно указать опцию конфигурации `auto_merge_output={action=merge}`, и тогда библиотека сама будет укрупнять получающиеся таблицы, если в них слишком мелкие чанки.

## MapReduce vs Map+Sort+Reduce

Подробнее про устройство операции MapReduce, а также про то, почему она обычно быстрее цепочки Map+Sort+Reduce, в разделе [MapReduce](../../../user-guide/data-processing/operations/mapreduce.md). 

Далее будут описаны случаи, когда **не рекомендуется** использовать слитную операцию MapReduce. 

### Тяжёлый и сильно фильтрующий mapper

В качестве условного ориентира можно принять то, что тяжёлый mapper тратит более 100мс CPU на обработку одной строки. Для сильно фильтрующего mapper-а характерно, если объем входных данных в пять и более раз превышает объём выходных данных по строкам или байтам.

В таком случае необходимо, чтобы в map-фазе было как можно больше джобов, чтобы каждый из них выполнялся быстрее. У операции MapReduce существует ограничение на количество map-джобов, вызванное тем, что если их слишком много, то после из-за большого количества мелких случайных чтений с диска будет дорого выполнять фазу сортировки.

Правильное решение в данном случае — сначала запустить операцию Map, указав как можно больше джобов. После этого на полученных данных запустить слитную операцию MapReduce с пустым mapper-ом.

### Частое использование MapReduce по одному и тому же набору ключевых полей

Если данные необходимо обработать в Reduce несколько раз, то скорее всего будет выгоднее предварительно их отсортировать, и после этого выполнять обычные Reduce-операции. Типичный случай таких данных — логи.

Данная схема будет хорошо работать только в двух случаях:

1. Данные не меняются.
2. Данные дописываются в конец (append). Например, таблица отсортирована по времени. Приходит дополнительная порция данных, для которой все значения ключа (времени) больше, чем все значения ключа (времени) в таблице. Тогда порцию данных можно отсортировать, после запустить Reduce с параметром `teleport=%true` Подробнее об опции в разделе [Reduce](../../../user-guide/data-processing/operations/reduce.md#foreign_tables).

### Тяжёлый reducer

Аналогично случаю с тяжёлым mapper-ом, требуется запустить как можно больше reduce-джобов. Но у операции существует ограничение на количество партиций, а значит и на количество reduce-джобов.

Правильное решение в данном случае — это отсортировать таблицу, а затем запускать Reduce с как можно большим количеством джобов.

## Большое количество записей с одинаковым ключом в reduce-фазе

Для борьбы с данной проблемой в {{product-name}} существуют [Reduce-комбайнеры](../../../user-guide/data-processing/operations/mapreduce#reduce_combiner.md), которые позволяют обрабатывать большие ключи в нескольких джобах в reduce-фазе.

### Map-комбайнеры

Идея map-комбайнера заключается в том, чтобы агрегировать данные в map-фазе. 

Классический пример использования map-комбайнеров — это [задача WordCount](http://wiki.apache.org/hadoop/WordCount). В данной задаче на map-стадии необходимо выписывать не пары `(word, 1)`, а пары `(word, count)`, где `count` — количество вхождений данного слова в рамках данного джоба. То есть агрегацию данных полезно выполнять не только в reduce-фазе, но и сразу в map-фазе.

Специальной поддержки map-комбайнеров в системе {{product-name}} нет. Причиной больших ключей зачастую является отсутствие агрегации в map-стадии. Поэтому если агрегация возможна, то рекомендуется её выполнять.

## Большое число выходных таблиц операции

Для каждой выходной таблицы операции резервируется буфер в оперативной памяти. На этапе запуска операции размер буферов под все выходные таблицы суммируется и прибавляется к тому объёму памяти, который пользователь указал в спецификации операции. 

Технически размер буфера под выходные таблицы неявно учитывается в памяти процесса `JobProxy` и зависит от настроек кластера, а также значения атрибута `erasure_codec` выходной таблицы. Подробнее про `JobProxy` в разделе 

Если выходных таблиц много, суммарный объём памяти может оказаться довольно большим, так что планировщик не сможет быстро найти узел кластера с подходящим объёмом свободной памяти, а может не найти его вовсе. В таком случае операция будет прервана, пользователь получит ошибку `No online node can satisfy the resource demand`. В сообщении об ошибке будет указано, какой объём памяти был запрошен. 

{% note info "Примечание" %}

Рекомендуется указывать не более нескольких десятков выходных таблиц для операции.

{% endnote %}
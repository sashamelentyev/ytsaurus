# YSON-документ

В данном разделе содержится информация о YSON-документах.

## Общие сведения { #common }

YSON-документ — это узел [Кипариса](../../../user-guide/storage/cypress.md), имеющий тип  **document** и предназначенный для хранения произвольных YSON-структур.

Помимо YSON-документа, создавать и хранить иерархические структуры данных можно в виде узлов типа `map_node`, `list_node`, `string_node` и других.
По сравнению с этим способом, YSON-документы имеют следующие отличия:

- Хранение данных в виде документа более компактно с точки зрения потребляемой памяти, так как узел документа не является полноценным узлом Кипариса.
- Документ ведет себя как единое целое с точки зрения специфичных для Кипариса возможностей: блокировок, владельцев, атрибутов  `revision`, `creation_time`, `modification_time`, `expiration_time` и других.

Работать с документами можно с помощью стандартных [команд](../../../api/commands.md):  `get`, `list`, `exists`, `set`, `remove`, как и с другими объектами в Кипарисе.

Поддерживаются запросы и модификации внутри самого документа. Адресация внутри документа осуществляется с помощью языка [YPath](../../../user-guide/storage/ypath.md).

Подробнее про формат [YSON](../../../user-guide/storage/yson.md).

## Использование { #usage }

Создать YSON-документ:

CLI
```bash
yt create document //tmp/my_test_doc
3f08-5b920c-3fe01a5-e0c12642
```

По умолчанию при создании в документе хранится YSON-entity. Прочитать YSON-документ:

CLI
```bash
yt get //tmp/my_test_doc
```

Указать начальное значение YSON-документа при создании:

CLI
```bash
yt create document //tmp/my_test_doc --attributes '{value=hello}'
3f08-6c0ee0-3fe01a5-2c4f6104
yt get //tmp/my_test_doc
```
```
"hello"
```

Записать в YSON-документ число и прочитать его:

CLI
```bash
yt set //tmp/my_test_doc 123
#
yt get //tmp/my_test_doc
123
```

Записать в YSON-документ сложную структуру и прочитать ее полностью или частично:

CLI
```bash
yt set //tmp/my_test_doc '{key1=value1;key2={subkey=456}}'
#
yt get //tmp/my_test_doc
```
```
{
    "key1" = "value1";
    "key2" = {
        "subkey" = 456;
    };
}
yt get //tmp/my_test_doc/key2
{
    "subkey" = 456;
}
```

При этом узел будет иметь тип `document`:

CLI
```bash
yt get //tmp/my_test_doc/@type
```
```
"document"
```

Частично изменить YSON-документ:

CLI
```bash
yt set //tmp/my_test_doc/key1 newvalue1
```

Удалить YSON-документ полностью:

CLI
```bash
yt remove //tmp/my_test_doc
```

## Ограничения { #limits }

В Кипарисе существует еще одна возможность создавать и хранить иерархические структуры данных: в виде узлов типа `map_node`, `list_node`, `string_node` и другие.

Чтение и запись YSON-документов происходит через мастер-сервер Кипариса, поэтому их нельзя применять в качестве высоконагруженной объектной базы данных. Разумный лимит — единицы [RPS](https://en.wikipedia.org/wiki/Queries_per_second). Поскольку эти данные хранятся в памяти мастер-сервера в виде дерева, следует с осторожностью выбирать объем данных.

Разумным лимитом на отдельный документ можно считать килобайты. Суммарный объем всех документов пользователя не должен превышать единицы мегабайт. Обычно такие узлы используются для хранения небольших кусочков структурированных метаданных, конфигурации и т.д.

## Системные атрибуты { #attributes }

Помимо атрибутов, присущих всем узлам Кипариса, документы имеют следующие дополнительные атрибуты:

| **Атрибут** | **Тип** | **Описание**                                                 |
| ----------- | ------- | ------------------------------------------------------------ |
| `value`     | `any`   | Полное содержимое документа. Атрибут позволяет задать содержимое документа при создании. Атрибут является [`opaque`](../../../user-guide/storage/attributes.md#system_attr), то есть при чтении всех атрибутов узла без фильтра он будет отображаться как [`entity`](../../../user-guide/storage/yson.md#entity). |

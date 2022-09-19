package yt

import (
	"context"
)

type userInfo struct {
	user  string
	token string
}

var userInfoKey userInfo

func enrichContextWithUserInfo(ctx context.Context, userLogin string, userToken string) context.Context {
	return context.WithValue(ctx, &userInfoKey, userInfo{user: userLogin, token: userToken})
}

func ReadUserTokenFromContext(ctx context.Context) (userToken string, ok bool) {
	if v := ctx.Value(&userInfoKey); v != nil {
		userToken = v.(userInfo).token
		ok = userToken != ""
	}
	return
}
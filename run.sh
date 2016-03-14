#!/bin/sh

command -v docker >/dev/null 2>&1|| \
  curl -s https://get.docker.io/ubuntu/ | sudo sh


[ ! -z $(docker images -q  yhpark/pintos-kaist) ] || \
  (docker pull yhpark/pintos-kaist && wget http://cps.kaist.ac.kr/courses/2016_spring_cs330/sources/pintos.tar.gz && \ 
tar xzf pintos.tar.gz && docker run -i -t -v $PWD:/pintos yhpark/pintos-kaist "/pintos/.script/check.sh")

docker run -i -t -v $PWD:/pintos yhpark/pintos-kaist /pintos/.script/entry.sh

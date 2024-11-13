FROM alpine:latest AS build
WORKDIR /
COPY . .
RUN apk add --update --no-cache make git gcc musl-dev && \
    make && \
    rm -rf /var/cache/apk/*

FROM alpine:latest
RUN apk add --update --no-cache busybox-extras && rm -rf /var/cache/apk/*
COPY --from=build /nyancat/src/nyancat /nyancat
COPY inetd.conf /etc/inetd.conf
COPY docker-entrypoint.sh /
RUN chmod 755 docker-entrypoint.sh && \
    chmod 755 /nyancat && \
    chown -R nobody:nobody /docker-entrypoint.sh /etc/inetd.conf /nyancat

RUN echo "net.ipv6.bindv6only=0" >> /etc/sysctl.conf

USER nobody
EXPOSE 2323
ENTRYPOINT ["/docker-entrypoint.sh"]
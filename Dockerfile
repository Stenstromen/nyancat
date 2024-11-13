FROM alpine:latest AS build
WORKDIR /
COPY src/*.c src/*.h src/Makefile src/
COPY Makefile .
RUN apk add --update --no-cache make git gcc musl-dev && \
    LDFLAGS="-static" make && \
    rm -rf /var/cache/apk/*

FROM scratch
COPY --from=build /src/nyancat /nyancat
EXPOSE 2323
USER 65534:65534
CMD ["/nyancat", "-l", "-p", "2323"]
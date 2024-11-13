# Nyancat CLI with built-in telnet server

**Shamelessly stolen from [github.com/klange/nyancat](https://github.com/klange/nyancat)**

**This release is hardcoded to fit in a 140x40 terminal window, this is done to avoid the need for a psuedo-terminal at the backend.**

Nyancat rendered in your terminal.

[![Nyancats](https://nyancat.dakko.us/nyancat.png)](https://nyancat.dakko.us/nyancat.png)

## Highly secure Docker image

### Run the Docker image

```bash
docker run -d --name nyancat -p 2323:2323 ghcr.io/stenstromen/nyancat:latest
```

### Build the Docker image

A Dockerfile is provided to build a highly secure Docker image.

```bash
docker build -t nyancat .
```

## Setup

First build the C application:

```bash
make && cd src
```

You can run the C application standalone.

```bash
./nyancat
```

To use the **new, built-in telnet server**, run:

```bash
nyancat -l -p 2323
```

## Distribution Specific Information

### Debian/Ubuntu

Debian and Ubuntu provide the nyancat binary through the `nyancat` package. A
`nyancat-server` package is provided to automatically setup and enable a nyancat
telnet server upon installation. I am not the maintainer of these packages;
please direct any questions or bugs to the relevant distribution's bug tracking
system.

## Licenses, References, etc

The original source of the Nyancat animation is
[prguitarman](http://www.prguitarman.com/index.php?id=348).

The original code ([github.com/klange/nyancat](https://github.com/klange/nyancat)) is licensed under the [NCSA license](http://en.wikipedia.org/wiki/University_of_Illinois/NCSA_Open_Source_License).

# Symbol-Type Graph (STG)

The STG (symbol-type graph) is an ABI representation and this
project contains tools for the creation and comparison of such
representations. At present parsers exist for libabigail's ABI XML
(C types only) and BTF. The ABI diff tool, stgdiff, supports multiple
reporting options.

This software currently depends on libxml2 for XML parsing, on libelf
to find .BTF sections and on Linux UAPI headers for BTF types.

## How to build the project

To build from source, you will need a few dependencies:

| *Debian*       | *RedHat*       |
| -------------- | -------------- |
| libelf-dev     | elfutils-devel |
| libxml2-dev    | libxml2-devel  |
| linux-libc-dev | kernel-headers |

Instructions are included for local and Docker builds.

### Local Build

You can build as follows:

```bash
$ make
```

### Docker Build

A [Dockerfile](Dockerfile) is provided to build a container with
libabigail to easily compile the STG tools:

```bash
$ docker build -t stg .
```

And then enter the container:

```bash
$ docker run -it stg
```

If you want to bind your development code to the container:

```bash
$ docker run -it $PWD:/src -it stg
```

The source code is added to `/src`, so when your code is bound you can
edit on your host and re-compile in the container.

Note that the Dockerfile can provide a development environment (non
multi-stage build with the source code) or a production image (a
multi-stage build with only the final binary).  By default we provide
the first, and you can uncomment the final lines of the file for the
latter.

## Contributions

See [CONTRIBUTING.md](CONTRIBUTING.md) for details.

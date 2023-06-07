# Symbol-Type Graph (STG)

The STG (symbol-type graph) is an ABI representation and this
project contains tools for the creation and comparison of such
representations. At present parsers exist for libabigail's ABI XML
(C types only) and BTF. The ABI diff tool, stgdiff, supports multiple
reporting options. This software currently depends on libabigail for
ELF symbol functionality, on libxml2 for XML parsing and on Linux UAPI
headers for BTF types.

## How to build the project

### Local Build

To build from source, you will need several dependencies on your host,
mentioned in the [Dockerfile](Dockerfile). Importantly, you will need
a cloned and compiled libabigail, which can be done as follows:

```bash
git clone git://sourceware.org/git/libabigail.git cd libabigail
```

Note that if you want to checkout a particular tag, you can do `git
tag` and then `git checkout <tag>` to ensure you aren't working with
a moving target. Next, prepare your build directory, and build:

```bash
mkdir -p build autoreconf -i cd build VISIBILITY_FLAGS=default
../configure --prefix=/opt/libabigail make -j $(nproc) all make install
```

In the above, setting the visibility flags variable is important as
will not work without it - symbols that need to be found in the final
compiled library will be hidden, and the build of STG tools will fail
due to missing symbols:

```bash
undefined reference to
`abigail::symtab_reader::symtab::make_filter() const' undefined
reference to `abigail::elf_helpers::find_section(Elf*, ...  collect2:
error: ld returned 1 exit status make: *** [Makefile:17: stgdiff]
Error 1
```

Specifically the symtab reader is an internal header and internal source file,
and the interface is not exposed. You can read more about visibility in
libabigail [here](https://sourceware.org/git/?p=libabigail.git;a=commit;h=103a6eb94faee7950fa57e4becffd453d7e4d1f7).
Note that this particular issue has been reported to the libabigail
maintainers, specifically a suggestion that these functions should
not be private by default.

You can then provide the libabigail source path to the
[Makefile](Makefile) as follows. Let's say you installed libabigail
to the path `/abigail`, you might run:

```bash
LIBABIGAIL_SRC=/abigail make -j $(nproc)
```

And that's it! If you want a portable environment ready to go, it's
recommended to build the container.

### Docker

A [Dockerfile](Dockerfile) is provided to build a container with
libabigail to easily compile the STG tools:

```bash
$ docker build -t stg .
```

And then enter the container:

```bash
$ docker run -it stg
```

Libabigail (a current dependency) is on the path, and the code for
the repository should be in the present working directory.  You can
build as follows:

```bash
$ make
```

Note that libabigail has source code and a build under `/code`,
and this path is default in the Makefile. If you want to bind your
development code to the container:

```bash
$ docker run -v $PWD:/src -it stg
```

And then you can make changes on the host and build / test in the
container.

## Contributions

See [CONTRIBUTING.md](CONTRIBUTING.md) for details.

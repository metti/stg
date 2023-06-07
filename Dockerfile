ARG debian_version=stable-slim
FROM debian:${debian_version} as builder
# docker build -t stg .
RUN apt-get update && \
    apt-get install -y build-essential \
    libelf-dev \
    linux-libc-dev \
    libxml2-dev
WORKDIR /src
COPY . /src
RUN make
# uncomment for multistage build
# FROM debian:${debian_version}
# COPY --from=builder /src/stgdiff /usr/local/bin/stgdiff

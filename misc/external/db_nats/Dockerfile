# Container image for the db_nats reference backend.
#
# Deliberately standalone: it is not referenced by the Kamailio build system,
# which has no Go toolchain. Build it from this directory.
#
#   docker build -t db_nats_backend .
#   docker run --rm -v $PWD/config.yaml:/etc/db_nats/config.yaml:ro db_nats_backend
#
# The image contains no shell or package manager, so configuration is mounted
# rather than baked in, and secrets stay outside the image.

FROM golang:1.24-alpine AS build

WORKDIR /src

# dependencies first, so they stay cached while only sources change
COPY go.mod go.sum ./
RUN go mod download

COPY *.go ./

# CGO is not needed: every dependency is pure Go, which is what allows the
# static runtime image below. -trimpath keeps build paths out of the binary.
RUN CGO_ENABLED=0 GOOS=linux go build \
        -trimpath \
        -ldflags="-s -w" \
        -o /out/db_nats_backend .

# Static distroless: no shell, no package manager, runs unprivileged.
FROM gcr.io/distroless/static-debian12:nonroot

COPY --from=build /out/db_nats_backend /usr/local/bin/db_nats_backend

# The binary embeds tzdata (time/tzdata in main.go), so mysql.timezone works
# even though this image carries no system zoneinfo.
USER nonroot:nonroot

ENTRYPOINT ["/usr/local/bin/db_nats_backend"]
CMD ["-config", "/etc/db_nats/config.yaml"]

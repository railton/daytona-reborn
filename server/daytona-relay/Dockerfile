# daytona-relay: the Online mode server for Daytona USA (Revision A).
# Go pinned by version and digest (a supported release: security fixes);
# raised with each Go point release.
FROM golang:1.27.4@sha256:5f1b98fa02fe396ebce2a0227a0e08309e11792915b9dd2e1f71053d7e7e6666 AS build
WORKDIR /src
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /daytona-relay ./cmd/daytona-relay

FROM scratch
COPY --from=build /daytona-relay /daytona-relay
USER 65534:65534
EXPOSE 7777/tcp 7777/udp
ENTRYPOINT ["/daytona-relay"]

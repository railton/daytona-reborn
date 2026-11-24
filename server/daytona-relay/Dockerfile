# daytona-relay: the Online mode server for Daytona USA (Revision A).
FROM golang:1.24 AS build
WORKDIR /src
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /daytona-relay ./cmd/daytona-relay

FROM scratch
COPY --from=build /daytona-relay /daytona-relay
USER 65534:65534
EXPOSE 7777/tcp
ENTRYPOINT ["/daytona-relay"]

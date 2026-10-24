# Build stage
FROM gcc:13 AS build
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY tools ./tools
RUN make gtpu-fwd

# Runtime stage (small image, non-root user)
FROM debian:bookworm-slim
RUN useradd --system --no-create-home gtpu
COPY --from=build /src/gtpu-fwd /usr/local/bin/gtpu-fwd
COPY routes.example.conf /etc/gtpu/routes.conf
USER gtpu
EXPOSE 2152/udp
ENTRYPOINT ["gtpu-fwd", "-r", "/etc/gtpu/routes.conf"]

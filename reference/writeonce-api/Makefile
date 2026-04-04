# Variables
CARGO=cargo
BUILD_DIR=target/lambda
BINARIES=lambda-post-article

# Default target
all: build

# Build all Lambda functions
build:
	@for bin in $(BINARIES); do \
		$(CARGO) lambda build --release --bin $$bin --output-format zip; \
	done

# Clean up build
clean:
	$(CARGO) clean

start-emulator:
	$(CARGO) lambda start &


run-post:
	$(CARGO) lambda invoke lambda-post-article --data-file events/post.json

dev-run-post: start-emulator run-post


# tools-rs/Makefile — thin cargo wrapper mirroring the C tools' make flow
# (all / android / clean), so CI and reviewers drive Rust the same way.
CARGO        ?= cargo
# Matches the C tools' android target (NDK r27 clang, bionic). The Docker image
# sets CC_/LINKER env for this target; see docker/Dockerfile.
ANDROID_TGT  ?= aarch64-linux-android

.PHONY: all android clean test

all:
	$(CARGO) build --release --manifest-path $(CURDIR)/Cargo.toml

android:
	$(CARGO) build --release --target $(ANDROID_TGT) \
		--manifest-path $(CURDIR)/Cargo.toml

test:
	$(CARGO) test --manifest-path $(CURDIR)/Cargo.toml

clean:
	$(CARGO) clean --manifest-path $(CURDIR)/Cargo.toml

# tests/host/Makefile — host-test build orchestration
.PHONY: helpers fixtures clean

helpers:
	$(MAKE) -C helpers

fixtures: helpers
	# Real PE fixtures are produced by scripts/extract-pe-from-fv.sh; we
	# don't regenerate them here. Synthetic fixtures land in tasks 1.13/1.15.

clean:
	$(MAKE) -C helpers clean
	rm -rf .last fixtures/poisoned-pe.efi fixtures/golden-payload.bin \
	       fixtures/synthetic-efisp.img

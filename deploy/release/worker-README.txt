vrp-worker @VERSION@, @PLATFORM@
====================================

@DEVICE@

Run it with the configuration and CA certificate that the control plane's
admin tool makes for this machine (see the deploy bundle's README.txt):

    vrp-worker --config worker.json

It logs "registered" once connected. To run it as a service, which restarts
it after a stall or a lost GPU, use the service file here as described in:
@REPOSITORY@/blob/@TAG@/deploy/README.md#run-it-as-a-service

THIRD_PARTY_NOTICES.txt lists the software built into it.

# png
png:
	@rm -rf fuzz/png/tmp/
	@mkdir fuzz/png/tmp/
	./fuzzer.py -i fuzz/png/out/queue/ -o fuzz/png/tmp ./programs/png/target/debug/png @@

png-afl:
	@afl-fuzz -i fuzz/png/in -o fuzz/png/out ./programs/target/debug/png @@

png-instrument:
	@cd programs/png && ./instrument.sh

png-clean:
	@cd programs/png && rm -rf target/


# zenoh
zenoh:
	@rm -rf fuzz/zenoh/tmp/
	@mkdir fuzz/zenoh/tmp/
	./fuzzer.py -i fuzz/zenoh/out/queue/ -o fuzz/zenoh/tmp ./programs/zenoh/target/release/examples/z_delete @@

zenoh-afl:
	@afl-fuzz -i fuzz/zenoh/in -o fuzz/zenoh/out ./programs/zenoh/target/release/examples/z_delete @@

zenoh-instrument:
	@cd programs/zenoh/zenoh && ./instrument.sh

zenoh-clean:
	@cd programs/zenoh && rm -rf target/


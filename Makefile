all: png

png:
	@rm -rf fuzz/png/tmp/
	@mkdir fuzz/png/tmp/
	./fuzzer.py -i fuzz/png/out/queue/ -o fuzz/png/tmp ./programs/target/debug/png @@

png-afl:
	@afl-fuzz -i fuzz/png/in -o fuzz/png/out ./programs/target/debug/png @@

instrument:
	@cd programs/ && ./instrument.sh

clean:
	@cd programs && rm -rf target/

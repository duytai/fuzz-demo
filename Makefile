all:
	@rm -rf tmp/
	@mkdir tmp/
	./fuzzer.py -i out/queue/ -o tmp/ ./pnm/target/debug/pnm @@

afl:
	@afl-fuzz -i in/ -o out/ ./pnm/target/debug/pnm @@

instrument:
	@cd pnm/ && ./instrument.sh

clean:
	@cd pnm && rm -rf target/

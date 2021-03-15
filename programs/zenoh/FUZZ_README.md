# Fuzz Zenoh with Rust-fuzzer

To show how to fuzz API of `zenoh` with Rust-fuzzer, we take `z_delete.rs` in folder `zenoh/zenoh/examples/zenoh/` as our fuzzing example. 

### Configurations

* Download [eclipse-zenoh/zenoh](https://github.com/eclipse-zenoh/zenoh) in folder `rust-fuzzer/programs/`.
```shell
cd rust-fuzzer/programs
git clone https://github.com/eclipse-zenoh/zenoh.git
```
* Copy `libafl-llvm-rt.a` and `instrument.sh` to `rust-fuzzer/programs/zenoh/zenoh`.
```shell
cd zenoh/zenoh
cp ../../png/instrument.sh .
cp ../../png/libafl-llvm-rt.a .
```

* Modify `instrument.sh`, change `cargo build` in the last line to `cargo build --release --all-targets`.

* Modify `z_delete.rs`. We simply test function `delete` in `z_delete.rs` which makes `path` as input. 

* To place the testcases and generated files of fuzzing, create folders `in` and `out` under the path of `rust-fuzzer/fuzz/`, and write testcases to the folder `in`.
```shell
cd ../../../ # change the directory to rust-fuzzer
mkdir -p fuzz/zenoh/in
mkdir fuzz/zenoh/out
echo "hello world" >fuzz/zenoh/in/test # Actually you can write everything reasonable.
```

* Modify `Makefile`.

### Start Fuzzing

```shell
make zenoh-clean
make zenoh-instrument # instrument this project.
make zenoh-afl # Generate the training data. Stop AFL whenever you think data is sufficient.
make zenoh # run our rust-fuzzer.
```

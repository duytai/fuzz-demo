# Introduction

This is a demo for rust-fuzzer. The architecture of the project is briefly described as following:

- Folder `pnm/` is a `Rust` project (https://github.com/image-rs/image). It takes `png` image and decode it
- Foder `in/` contains initial testcases
- Folder `verifier/` is a simplified version of a AFL fuzzer. It reads all testcases from a folder and generates meaningful reports (e.g. edge coverage, number of paths, etc...)
- File `fuzzer.py` contains the main logic of our fuzzer:
  - **Step1**: Read testcases, bitmap to create train data
  - **Step2**: Detect hot bytes and brute-force them to generate testcases (under `tmp/topk/`)
  - **Step3**: Ask `veifier` to execute newly generated testcases (under `tmp/topk`)
  - **Step4**: Jump to **Step1**
  
# Requirements

To run this, you have to install the following packages:

- pytorch: ```pip install torch```
- numpy: ```pip install numpy```

Note that: rust-fuzzer requires GPU


# How to run

Fuzz `pnm/` project with our rust-fuzzer:
```javascript
make
```

This project is ready to fuzz. If you want to re-run the entire process. Plz, do the following steps:

```bash
make clean
make instrument # instrument Rust project
make afl # run AFL to generate training data. Whenever you think data is sufficient, stop AFL
make # run our rust-fuzzer

```

# Sample
![](https://s8.gifyu.com/images/ezgif-6-28948025d975.gif)

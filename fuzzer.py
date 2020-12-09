#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import numpy as np
import os
import glob
import torch
import torch.nn as nn
import torch.nn.functional as F
import math
import matplotlib.pyplot as plt
import random
import sys
import subprocess
import shutil
import logging
from tqdm import tqdm

logging.basicConfig(level='INFO')
log = logging.getLogger("fuzzer")

verifier = "./verifier/verifier"
MAP_SIZE = 1 << 16
batch_size = 32
total_epoch = 500

## feedforward
class Net(nn.Module):
    def __init__(self, in_size, out_size):
        super(Net, self).__init__()
        layer_size = [in_size, 4096, out_size]
        self.fc1 = nn.Linear(layer_size[0], layer_size[1])
        self.do1 = nn.Dropout(p=0.2)
        self.fc2 = nn.Linear(layer_size[1], layer_size[2])

    def forward(self, x):
        x = torch.relu(self.do1(self.fc1(x)))
        x = torch.sigmoid(self.fc2(x))
        return x

## program entry
if __name__ == "__main__":
    
    ### parse arguments
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] == "-i":
            assert i + 1 < len(argv)
            in_dir = argv[i + 1]
            i += 2
            continue
        if argv[i] == "-o":
            assert i + 1 < len(argv)
            out_dir = argv[i + 1]
            i += 2
            continue
        break
    target = argv[i:]
    
    ### stats
    total_execs = 0
    total_paths = 0
    total_crashes = 0
    
    ### dirs
    loss_dir = "%s/loss_bits" % out_dir
    seed_dir = "%s/seeds" % out_dir
    queue_dir = "%s/queue" % out_dir
    map_file = "%s/virgin_bit" % out_dir
    report_file = "%s/report_file" % out_dir
    
    ## copy to queue dir no map_file
    files = []
    ### create new labels
    subprocess.run([
        verifier, "-i", in_dir, "-t", loss_dir, "-m", map_file, "-r", report_file
    ] + target)
    ## create queue_dir
    os.mkdir(queue_dir)
    ## read and save to queue_dir
    files = open(report_file, "r").read().strip().split("\n")
    for idx, file in enumerate(files):
        res, hnb, file = int(file[0]), int(file[2]), file[4:]
        shutil.copy(file, queue_dir)
        total_paths += 1
        total_crashes += res == 2
    
    while True:
        
        ### read datasets
        loss_files = sorted(glob.glob("%s/id:*" % loss_dir))
        in_files = list(map(lambda x: "%s/%s" % (queue_dir, x.split('/')[-1]), loss_files))
        in_sizes = list(map(lambda x: os.path.getsize(x), in_files))
        max_size = max(in_sizes)
        in_count = len(loss_files)

        loss_bytes = np.zeros((in_count, MAP_SIZE), dtype=np.uint8)
        in_bytes = np.zeros((in_count, max_size), dtype=np.uint8)

        virgin_loss = np.full(MAP_SIZE, 0, dtype=np.uint8)

        for idx, in_file, loss_file in list(zip(range(in_count), in_files, loss_files)):
            in_byte = np.frombuffer(open(in_file, "rb").read(), dtype=np.uint8)
            in_bytes[idx][0:in_byte.shape[0]] = in_byte

            loss_byte = np.frombuffer(open(loss_file, "rb").read(), dtype=np.uint8).copy()
            loss_byte[loss_byte > 1] = 1
            virgin_loss = virgin_loss | loss_byte
            loss_bytes[idx] = loss_byte

        branches = np.where(virgin_loss == 1)[0]
        branch_count = branches.shape[0]
        total_execs += in_count

        ## form labels
        loss_norms = np.zeros((in_count, branch_count), dtype=np.float64)
        for idx, loss_byte in zip(range(in_count), loss_bytes):
            loss_norms[idx] = loss_byte[branches]
        in_norms = in_bytes / 255.0

        ## stats
        density = len(loss_norms[loss_norms != 0]) / (loss_norms.shape[0] * loss_norms.shape[1])
        
        tmp = np.frombuffer(open(map_file, "rb").read(), dtype=np.uint8)
        log.info("num inputs\t: %d" % in_count)
        log.info("file size\t: %d" % max_size)
        log.info("density\t: %.02f" % (density * 100))
        log.info("coverage\t: %d" % len(tmp[tmp != 255]))
        
        ## create neural network
        log.info('load model...')
        device = torch.device('cuda')
        net = Net(in_norms.shape[1], loss_norms.shape[1]).double().to(device)
        loss_fn = torch.nn.BCELoss()
        optimizer = torch.optim.Adam(net.parameters(), lr=1e-4)

        ## train them
        in_norms_train = in_norms[0: int(0.9 * in_norms.shape[0])]
        in_norms_test = in_norms[int(0.9 * in_norms.shape[0]):]

        loss_norms_train = loss_norms[0: int(0.9 * loss_norms.shape[0])]
        loss_norms_test = loss_norms[int(0.9 * loss_norms.shape[0]):]

        xs_train = torch.tensor(in_norms_test, device=device)
        ys_train = torch.tensor(loss_norms_test, device=device)

        xs_test = torch.tensor(in_norms_train, device=device)
        ys_test = torch.tensor(loss_norms_train, device=device)

        log.info('training...')
        for epoch in tqdm(range(total_epoch)):
            for at in range(0, xs_train.shape[0], batch_size):
                y_pred = net(xs_train[at: at + batch_size])
                loss = loss_fn(y_pred, ys_train[at : at + batch_size])
                optimizer.zero_grad()
                loss.backward()
                optimizer.step()

        ### generate testcases

        if os.path.exists(seed_dir):
            shutil.rmtree(seed_dir)
        os.mkdir(seed_dir)

        in_norms_tensor = torch.tensor(in_norms, requires_grad = True, device=device)
        loss_norms_tensor = torch.tensor(loss_norms, device=device)
        y_pred = net(in_norms_tensor)
        loss = loss_fn(y_pred, loss_norms_tensor)
        optimizer.zero_grad()
        loss.backward()

        log.info('generating...')
        counter = 0
        with torch.no_grad():
            grads = in_norms_tensor.grad.cpu().numpy()
            for grad, in_byte, in_size in tqdm(list(zip(grads, in_bytes, in_sizes))):
                topk = np.abs(grad[:in_size]).argsort()[-5:][::-1]
                sign = np.sign(grad[topk])
                tmp = in_byte[topk]
                for m in range(255):
                    in_byte[topk] = (m + 1) * sign + tmp
                    in_byte[:in_size].tofile("%s/id:%010d" % (seed_dir, total_execs + counter))
                    counter += 1
                in_byte[topk] = tmp
            log.info("generated\t: %d" % counter)
        
        ## execute with newly generated testcases
        subprocess.run([verifier, "-i", seed_dir, "-t", loss_dir, "-m", map_file, "-r", report_file] + target)
        total_execs += counter

        ## read report and copy to in_dirs
        content = open(report_file, "r").read().rstrip()
        if len(content):
            files = content.split("\n")
            for idx, file in enumerate(files):
                res, hnb, file = int(file[0]), int(file[2]), file[4:]
                shutil.copy(file, queue_dir)
                total_paths += 1
                total_crashes += res == 2
                
            tmp = np.frombuffer(open(map_file, "rb").read(), dtype=np.uint8)
            log.info("found\t: %d", len(files))
            log.info("executed\t: %d", total_execs)
            log.info("crashes\t: %d", total_crashes)
            log.info("coverage\t: %d", len(tmp[tmp != 255]))

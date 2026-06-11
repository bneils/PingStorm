#!/bin/sh

# default compression is 6, higher is better
gzip -c ping.dat -4 > ping.dat.gz

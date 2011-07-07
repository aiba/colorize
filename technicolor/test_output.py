#!/usr/bin/python

import sys
import time

while (True):
  sys.stdout.write("Behold: This is a line written to stdout.\n");
  sys.stdout.write("This is another line written to stdout.\n");
  sys.stdout.flush()

  sys.stderr.write("Behold: This is a line written to stderr.\n");
  sys.stderr.write("This is another line written to stderr.\n");
  sys.stderr.flush()
  
  time.sleep(1)


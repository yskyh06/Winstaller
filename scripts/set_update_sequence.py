#!/usr/bin/env python3
"""Stamp a release counter without changing the configured public trust keys."""
import json
import os
from pathlib import Path
import sys
path=Path(sys.argv[1])
sequence=int(os.environ['WINSTALLER_RELEASE_SEQUENCE'])
if not 1<=sequence<=999999999:raise ValueError('Invalid release sequence')
config=json.loads(path.read_text())
config['release_sequence']=sequence
path.write_text(json.dumps(config,indent=2)+'\n')

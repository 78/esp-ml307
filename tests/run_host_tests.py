#!/usr/bin/env python3
"""Exercise production AtUart RX/TX methods with fake RTOS/UHCI primitives.

ASan/UBSan cover the adapter; real GDMA, ISR timing and UART rates need hardware.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, name):
    match = re.search(r'^.*AtUart::' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    depth = 0
    for token in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|[{}]', source[match.start():], re.S):
        if token[0] == '{':
            depth += 1
        if token[0] == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():match.start() + token.end()]
    raise AssertionError(name)


source = (ROOT / 'src/at_uart.cc').read_text()
methods = '\n\n'.join(method(source, name) for name in ('DmaRxCallback', 'ReceiveTask', 'SendData'))
text = (ROOT / 'tests/at_uart_test.cpp').read_text().replace('// @METHODS@', methods)
with tempfile.TemporaryDirectory(prefix='esp-ml307-test-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    cpp.write_text(text)
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-g', '-O1',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

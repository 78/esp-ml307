#!/usr/bin/env python3
"""Exercise production AtUart and HttpClient code with fake RTOS/transport primitives.

ASan/UBSan cover the adapters; real GDMA, ISR timing, UART rates and TLS need hardware.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CXX = os.environ.get('CXX', 'clang++')
SANITIZE = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']


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


def run_at_uart_test(tmp):
    source = (ROOT / 'src/at_uart.cc').read_text()
    methods = '\n\n'.join(method(source, name) for name in ('DmaRxCallback', 'ReceiveTask', 'SendData'))
    text = (ROOT / 'tests/at_uart_test.cpp').read_text().replace('// @METHODS@', methods)
    cpp = Path(tmp) / 'at_uart_test.cpp'
    cpp.write_text(text)
    binary = Path(tmp) / 'at_uart_test'
    subprocess.run([CXX, '-std=c++20', *SANITIZE, str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def run_http_client_test(tmp):
    binary = Path(tmp) / 'http_client_test'
    subprocess.run([CXX, '-std=c++23', *SANITIZE, '-pthread', '-Wall', '-Werror',
                    '-I', str(ROOT / 'tests/stubs'), '-I', str(ROOT / 'include'),
                    str(ROOT / 'tests/http_client_test.cpp'), str(ROOT / 'src/http_client.cc'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


with tempfile.TemporaryDirectory(prefix='esp-ml307-test-') as tmp:
    run_at_uart_test(tmp)
    run_http_client_test(tmp)

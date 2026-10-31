"""Keep the F1 memory disclosure tied to every added storage component."""
from pathlib import Path
import ast
import re

root = Path(__file__).resolve().parents[1]
source = (root / 'source/network/server_interface.c').read_text(encoding='utf-8')
definitions = dict(re.findall(r'^#define\s+(\w+)\s+([^\n]+)',
    re.sub(r'/\*.*?\*/', '', source.replace('\\\n', ''), flags=re.S), flags=re.M))
def integer(node):
    if isinstance(node, ast.Constant) and isinstance(node.value, int):
        return node.value
    if isinstance(node, ast.Name):
        return integer(ast.parse(definitions[node.id], mode='eval').body)
    if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Mult)):
        a, b = integer(node.left), integer(node.right)
        return a + b if isinstance(node.op, ast.Add) else a * b
    raise AssertionError('Unexpected memory-capacity expression: '+ast.dump(node))
capacity = integer(ast.parse(definitions['SVIN_PENDING_SIZE'], mode='eval').body)
assert capacity == 203, f'F1 storage footprint changed without a memory-budget ruling: {capacity}'
assert not re.search(r'85 bytes|85 B', source), 'F1 control flags are 203 elements, not 85'
doc = (root / 'tests/fixtures/f1_memory_inventory.md').read_text(encoding='utf-8').split('<!-- F1_MOVEMENT_CREDITS_2026_10_10 -->')[1]
for component in ('203', 'thread_mutex', 'credit counters', 'motion_credit', 'Wii', 'alignment', 'BSS', 'heap'):
    assert component.lower() in doc.lower(), f'F1 memory inventory omits {component}'
print('R4 finding 6: complete F1 bounded memory disclosure PASS')

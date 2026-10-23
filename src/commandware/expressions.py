"""Bounded prefix-call expressions shared by script argument parsing and execution."""
import math
import re

# Parameter order also defines the positional invocation contract.
SIGNATURES = {
    "number": ("x",), "int": ("x",), "string": ("x",), "abs": ("x",),
    "min": ("a", "b"), "max": ("a", "b"), "clamp": ("x", "low", "high"),
    "floor": ("x",), "ceil": ("x",), "round": ("x",), "notehz": ("text",),
    "len": ("x",), "contains": ("x", "item"), "trim": ("text",),
    "lower": ("text",), "upper": ("text",), "slice": ("x", "start", "end"),
    "replace": ("text", "old", "new"), "at": ("items", "index"),
    "join": ("items", "separator"),
}
MAX_STRING_BYTES = 4096
MAX_LIST_ITEMS = 64
ASCII_SPACE = " \t\r\n\v\f"


def checked_value(value):
    if isinstance(value, str):
        try:
            size = len(value.encode("utf-8"))
        except UnicodeEncodeError as exc:
            raise ValueError("invalid UTF-8 text") from exc
        if size > MAX_STRING_BYTES:
            raise ValueError("string exceeds 4096 UTF-8 bytes")
    elif isinstance(value, list):
        if len(value) > MAX_LIST_ITEMS:
            raise ValueError("list exceeds 64 items")
        for item in value:
            if isinstance(item, list):
                raise ValueError("nested lists are unsupported")
            checked_value(item)
    elif isinstance(value, (int, float)):
        numeric(value)
    return value


def text_value(value):
    if not isinstance(value, str):
        raise ValueError("expected string")
    return checked_value(value)


def list_value(value):
    if not isinstance(value, list):
        raise ValueError("expected list")
    return checked_value(value)


def value_index(value):
    if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0 or value != math.trunc(value):
        raise ValueError("index must be a nonnegative integer")
    return int(value)


def scalar_equal(a, b):
    if isinstance(a, str) and isinstance(b, str):
        return a == b
    return numeric(a) == numeric(b)
_NUMBER = r"(?:0[xX][0-9a-fA-F]+|(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)"
_NUMERIC = re.compile(r"[+-]?" + _NUMBER + r"\Z")
_PRECEDENCE = {"||": 1, "&&": 2, "==": 3, "!=": 3, "<": 4, "<=": 4, ">": 4, ">=": 4,
               "+": 5, "-": 5, "*": 6, "/": 6, "%": 6}


def numeric(value):
    if isinstance(value, str):
        text = value.strip(" \t\r\n\v\f")
        if not _NUMERIC.fullmatch(text):
            raise ValueError("invalid numeric text")
        signless = text.lstrip("+-")
        if signless.lower().startswith("0x"):
            try:
                value = float(int(text, 16))
            except OverflowError as exc:
                raise ValueError("nonfinite number") from exc
        else:
            value = float(text)
    if not isinstance(value, (float, int)):
        raise ValueError("expected numeric value")
    try:
        value = float(value)
    except OverflowError as exc:
        raise ValueError("nonfinite number") from exc
    if not math.isfinite(value):
        raise ValueError("nonfinite number")
    return value


def format_number(value):
    value = numeric(value)
    if value == 0:
        return "0"
    # Seventeen significant digits recover every binary64 value, matching firmware.
    return format(value, ".17g")


class Expression:
    def __init__(self, source, lookup=None, notehz=None):
        self.source = source
        self.pos = 0
        self.lookup = lookup
        self.notehz = notehz

    def space(self):
        while self.pos < len(self.source) and self.source[self.pos].isspace():
            self.pos += 1

    def option(self):
        self.space()
        match = re.match(r"--[A-Za-z_][A-Za-z_0-9]*", self.source[self.pos:])
        return match[0] if match else None

    def parse(self, minimum=0, depth=0):
        if depth > 16:
            raise ValueError("expression nesting exceeds 16")
        self.space()
        if self.pos >= len(self.source) or self.option():
            raise ValueError("missing expression value")
        c = self.source[self.pos]
        self.pos += 1
        if c in "+-!":
            left = ("unary", c, self.parse(7, depth + 1))
        elif c == "(":
            left = self.parse(0, depth + 1)
            self.space()
            if self.pos >= len(self.source) or self.source[self.pos] != ")":
                raise ValueError("missing expression closing parenthesis")
            self.pos += 1
            left = ("group", left)
        elif c in "\"'":
            parts, text = [], ""
            while self.pos < len(self.source):
                ch = self.source[self.pos]
                self.pos += 1
                if ch == c:
                    if text:
                        parts.append(("literal", text))
                    break
                if ch == "\\":
                    if self.pos == len(self.source):
                        raise ValueError("unterminated string")
                    ch = self.source[self.pos]
                    self.pos += 1
                    escapes = {"n": "\n", '"': '"', "'": "'", "\\": "\\", "$": "$"}
                    if ch not in escapes:
                        raise ValueError("unknown string escape")
                    text += escapes[ch]
                elif ch == "$":
                    match = re.match(r"[A-Za-z_][A-Za-z_0-9]*", self.source[self.pos:])
                    if not match:
                        raise ValueError("expected name after $")
                    if text:
                        parts.append(("literal", text))
                        text = ""
                    parts.append(("ref", match[0]))
                    self.pos += len(match[0])
                else:
                    text += ch
            else:
                raise ValueError("unterminated string")
            left = ("text", parts)
        elif c == "$":
            match = re.match(r"[A-Za-z_][A-Za-z_0-9]*", self.source[self.pos:])
            if not match:
                raise ValueError("expected name after $")
            self.pos += len(match[0])
            left = ("ref", match[0])
        elif c.isdigit() or c == ".":
            self.pos -= 1
            match = re.match(_NUMBER, self.source[self.pos:])
            if not match:
                raise ValueError("invalid number")
            self.pos += len(match[0])
            left = ("number", match[0])
        elif c.isascii() and (c.isalpha() or c == "_"):
            self.pos -= 1
            match = re.match(r"[A-Za-z_][A-Za-z_0-9]*", self.source[self.pos:])
            name = match[0].lower()
            self.pos += len(match[0])
            if name not in SIGNATURES:
                raise ValueError("invalid expression: read variable values with $name")
            if self.pos < len(self.source) and not self.source[self.pos].isspace():
                raise ValueError("function arguments require spaces")
            params = SIGNATURES[name]
            named = self.option() is not None
            args = {}
            for param in params:
                if named:
                    option = self.option()
                    if option is None:
                        raise ValueError("missing named function argument")
                    self.pos += len(option)
                    param = option[2:]
                    if param not in params or param in args:
                        raise ValueError("unknown or duplicate function argument")
                elif self.option():
                    raise ValueError("cannot mix positional and named function arguments")
                args[param] = self.parse(0, depth + 1)
            left = ("call", name, [args[param] for param in params])
        else:
            raise ValueError("invalid expression")
        compared = False
        while True:
            self.space()
            if self.option():
                break
            op = next((op for op in ("||", "&&", "==", "!=", "<=", ">=", "+", "-", "*", "/", "%", "<", ">")
                       if self.source.startswith(op, self.pos)), None)
            if op is None or _PRECEDENCE[op] < minimum:
                break
            if op in ("<", "<=", ">", ">=", "==", "!="):
                if compared:
                    raise ValueError("comparison chaining is unsupported")
                compared = True
            self.pos += len(op)
            right = self.parse(_PRECEDENCE[op] + 1, depth + 1)
            if op in ("==", "!=") and right[0] == "binary" and right[1] in ("<", "<=", ">", ">=", "==", "!="):
                raise ValueError("comparison chaining is unsupported")
            left = ("binary", op, left, right)
        return left

    def run(self, syntax_only=False):
        node = self.parse()
        self.space()
        if self.pos != len(self.source):
            raise ValueError("invalid expression")
        return 1.0 if syntax_only else self.evaluate(node)

    def evaluate(self, node, depth=0):
        if depth > 16:
            raise ValueError("expression nesting exceeds 16")
        kind = node[0]
        if kind == "group":
            return self.evaluate(node[1], depth + 1)
        if kind == "literal":
            return node[1]
        if kind == "number":
            return numeric(node[1])
        if kind == "ref":
            return checked_value(self.lookup(node[1]))
        if kind == "text":
            result = ""
            for part in node[1]:
                value = self.evaluate(part, depth + 1)
                result = checked_value(result + (value if isinstance(value, str) else format_number(value)))
            return result
        if kind == "unary":
            value = numeric(self.evaluate(node[2], depth + 1))
            return float(value == 0) if node[1] == "!" else (-value if node[1] == "-" else value)
        if kind == "call":
            name = node[1]
            args = [self.evaluate(arg, depth + 1) for arg in node[2]]
            if name == "string":
                return checked_value(args[0] if isinstance(args[0], str) else format_number(args[0]))
            if name == "len":
                if not isinstance(args[0], (str, list)):
                    raise ValueError("expected string or list")
                return float(len(checked_value(args[0])))
            if name == "contains":
                if isinstance(args[0], str):
                    return float(text_value(args[1]) in args[0])
                items = list_value(args[0])
                if isinstance(args[1], list):
                    raise ValueError("list comparison is unsupported")
                return float(any(scalar_equal(item, args[1]) for item in items))
            if name in ("trim", "lower", "upper"):
                text = text_value(args[0])
                if name == "trim":
                    return text.strip(ASCII_SPACE)
                source, target = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")
                if name == "upper":
                    source, target = target, source
                return text.translate(str.maketrans(source, target))
            if name == "slice":
                value = args[0]
                if not isinstance(value, (str, list)):
                    raise ValueError("expected string or list")
                start, end = value_index(args[1]), value_index(args[2])
                if start > end or end > len(value):
                    raise ValueError("invalid slice range")
                return checked_value(value[start:end])
            if name == "at":
                items, index = list_value(args[0]), value_index(args[1])
                if index >= len(items):
                    raise ValueError("index out of range")
                return items[index]
            if name == "replace":
                text, old, new = map(text_value, args)
                if not old:
                    raise ValueError("empty replacement search")
                count = text.count(old)
                if len(text.encode("utf-8")) + count * (len(new.encode("utf-8")) - len(old.encode("utf-8"))) > MAX_STRING_BYTES:
                    raise ValueError("string exceeds 4096 UTF-8 bytes")
                return checked_value(text.replace(old, new))
            if name == "join":
                items, separator = list_value(args[0]), text_value(args[1])
                parts = [text_value(item) for item in items]
                size = sum(len(part.encode("utf-8")) for part in parts) + max(0, len(parts) - 1) * len(separator.encode("utf-8"))
                if size > MAX_STRING_BYTES:
                    raise ValueError("string exceeds 4096 UTF-8 bytes")
                return separator.join(parts)
            if name == "notehz":
                if not isinstance(args[0], str):
                    raise ValueError("notehz requires text")
                return numeric(self.notehz(args[0]))
            args = [numeric(arg) for arg in args]
            if name == "clamp":
                if args[1] > args[2]:
                    raise ValueError("clamp: low exceeds high")
                return min(max(args[0], args[1]), args[2])
            operations = {"number": lambda x: x, "int": math.trunc, "abs": abs,
                          "min": min, "max": max, "floor": math.floor, "ceil": math.ceil,
                          "round": lambda x: math.copysign(math.floor(abs(x)) + (abs(x) % 1 >= 0.5), x)}
            return numeric(operations[name](*args))
        op = node[1]
        left = self.evaluate(node[2], depth + 1)
        if op == "&&" and numeric(left) == 0:
            return 0.0
        if op == "||" and numeric(left) != 0:
            return 1.0
        right = self.evaluate(node[3], depth + 1)
        if op in ("==", "!=") and isinstance(left, str) and isinstance(right, str):
            return float((left == right) if op == "==" else (left != right))
        left, right = numeric(left), numeric(right)
        if op in ("/", "%") and right == 0:
            raise ValueError("division by zero")
        operations = {"+": lambda: left + right, "-": lambda: left - right,
                      "*": lambda: left * right, "/": lambda: left / right,
                      "%": lambda: math.fmod(left, right), "<": lambda: left < right,
                      "<=": lambda: left <= right, ">": lambda: left > right,
                      ">=": lambda: left >= right, "==": lambda: left == right,
                      "!=": lambda: left != right, "&&": lambda: right != 0,
                      "||": lambda: right != 0}
        return numeric(operations[op]())

"""Bounded virtual-time counterpart of the firmware's cooperative script runtime."""
from __future__ import annotations

import ast
import math
import re
from dataclasses import dataclass, field
from typing import Generator

from .core import Command, HardwareState, JobManager, _clock_options, _dispatch, parse_schedule


@dataclass
class Token:
    text: str
    quoted: bool = False
    line: int = 1


@dataclass
class Stage:
    name: str
    args: list[Token] = field(default_factory=list)
    body: list[list[Stage]] = field(default_factory=list)
    line: int = 1


def located(message, line):
    return message if " at line " in message else f"{message} at line {line}"


def parse_script(source: str) -> list[list[Stage]]:
    if len(source.encode()) > 4096:
        raise ValueError("script exceeds 4096 bytes at line 1")
    tokens = []
    i = 0
    line = 1
    while i < len(source):
        token_line = line
        c = source[i]
        i += 1
        if c in " \t\r":
            continue
        if c == "#":
            while i < len(source) and source[i] != "\n":
                i += 1
        elif c in "\"'":
            quote, text = c, ""
            while i < len(source) and source[i] != quote:
                c = source[i]
                i += 1
                if c == "\n":
                    line += 1
                if c == "\\" and i < len(source):
                    c = source[i]
                    i += 1
                    if c == "\n":
                        line += 1
                    c = "\n" if c == "n" else c
                text += c
            if i == len(source):
                raise ValueError(located("unterminated string", token_line))
            i += 1
            tokens.append(Token(text, True, token_line))
        elif c in "{}|;\n":
            tokens.append(Token(c, line=token_line))
            if c == "\n":
                line += 1
        else:
            text = c
            while i < len(source) and not source[i].isspace() and source[i] not in "{}|;\"'":
                text += source[i]
                i += 1
            tokens.append(Token(text, line=token_line))
    pos = nodes = 0

    def fail(message):
        return ValueError(located(message, tokens[pos].line if pos < len(tokens) else line))

    def at(text):
        return pos < len(tokens) and not tokens[pos].quoted and tokens[pos].text == text

    def body(depth=0, entries=False):
        nonlocal pos, nodes
        if depth > 8:
            raise fail("block nesting exceeds 8")
        result = []
        while at("\n") or at(";"):
            pos += 1
        while pos < len(tokens) and not at("}"):
            pipe = []
            while True:
                if pos == len(tokens) or any(at(c) for c in ("}", "|", ";", "\n")):
                    raise fail("expected command")
                name = tokens[pos].text
                command_line = tokens[pos].line
                pos += 1
                nodes += 1
                if nodes > 128:
                    raise fail("script exceeds 128 commands")
                stage = Stage(name if entries else name.lower(), line=command_line)
                while pos < len(tokens) and not any(at(c) for c in ("{", "}", "|", ";", "\n")):
                    stage.args.append(tokens[pos])
                    pos += 1
                if at("{"):
                    pos += 1
                    stage.body = body(depth + 1, name == "schedule")
                    if not at("}"):
                        raise fail("missing closing brace")
                    pos += 1
                pipe.append(stage)
                if not at("|"):
                    break
                pos += 1
                while at("\n"):
                    pos += 1
            result.append(pipe)
            if pos < len(tokens) and not any(at(c) for c in ("}", ";", "\n")) and not entries:
                raise fail("expected statement separator")
            while at("\n") or at(";"):
                pos += 1
        return result

    result = body()
    if pos != len(tokens):
        raise fail("unexpected closing brace")
    return result


_UNSET = object()


@dataclass
class Event:
    value: object = _UNSET
    fields: dict[str, object] = field(default_factory=dict)
    timestamp: int = 0
    has_value: bool = field(init=False)

    def __post_init__(self):
        self.has_value = self.value is not _UNSET
        if not self.has_value:
            self.value = 0


def identifier(name):
    return bool(re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name)) and name != "value"


def notehz(note):
    match = re.fullmatch(r"([A-G])([#b]?)([0-8])", str(note))
    if not match:
        raise ValueError("invalid note")
    letter, accidental, octave = match.groups()
    midi = (int(octave) + 1) * 12 + {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}[letter]
    midi += {"": 0, "#": 1, "b": -1}[accidental]
    return 440 * 2 ** ((midi - 69) / 12)


def lookup(name, scope, event):
    if name == "value":
        if not event.has_value:
            raise ValueError("pipeline value is required")
        return event.value
    if name in scope and name in event.fields:
        raise ValueError(f"ambiguous name '{name}'")
    if name in scope:
        return scope[name]
    if name in event.fields:
        return event.fields[name]
    raise ValueError(f"unknown name '{name}'")


def expression(text, scope, event, syntax_only=False):
    # Python's invert operator has the same precedence as C-style unary !.
    # Map it explicitly rather than using Python's lower-precedence `not`.
    if "~" in text:
        raise ValueError("unsupported expression operator '~'")
    text = text.replace("!", "~")
    text = re.sub(r"\$([A-Za-z_][A-Za-z_0-9]*)", lambda m: f'ref("{m[1]}")', text)
    try:
        root = ast.parse(text, mode="eval")
    except SyntaxError as exc:
        raise ValueError("invalid expression") from exc

    def walk(node, depth=0):
        if depth > 16:
            raise ValueError("expression nesting exceeds 16")
        if isinstance(node, ast.Constant) and isinstance(node.value, (int, float, str)):
            return node.value
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and len(node.args) == 1 and not node.keywords:
            arg = walk(node.args[0], depth + 1)
            if node.func.id == "ref":
                return 1 if syntax_only else lookup(arg, scope, event)
            if node.func.id == "int":
                return 1 if syntax_only else int(float(arg))
            if node.func.id == "notehz":
                return 1 if syntax_only else notehz(arg)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub, ast.Invert)):
            value = float(walk(node.operand, depth + 1))
            if isinstance(node.op, ast.Invert):
                return int(value == 0)
            return -value if isinstance(node.op, ast.USub) else value
        if isinstance(node, ast.BinOp):
            left, right = float(walk(node.left, depth + 1)), float(walk(node.right, depth + 1))
            if syntax_only and isinstance(node.op, (ast.Add, ast.Sub, ast.Mult, ast.Div, ast.Mod)):
                return 1
            if isinstance(node.op, ast.Add):
                return left + right
            if isinstance(node.op, ast.Sub):
                return left - right
            if isinstance(node.op, ast.Mult):
                return left * right
            if isinstance(node.op, (ast.Div, ast.Mod)):
                if right == 0:
                    raise ValueError("division by zero")
                return left / right if isinstance(node.op, ast.Div) else math.fmod(left, right)
        raise ValueError("invalid expression: read variable values with $name")

    try:
        result = walk(root.body)
        if isinstance(result, (int, float)) and not math.isfinite(result):
            raise ValueError("invalid number")
        return result
    except (TypeError, OverflowError) as exc:
        raise ValueError("invalid expression value") from exc


def display(value):
    return f"{value:.12g}" if isinstance(value, (float, int)) else str(value)


def evaluate(token, scope, event):
    text = token.text
    if token.quoted:
        return re.sub(r"\$([A-Za-z_][A-Za-z_0-9]*)", lambda m: display(lookup(m[1], scope, event)), text)
    if expression_token(token):
        return expression(text, scope, event)
    return text


def expression_token(token):
    text = token.text
    return not token.quoted and not text.startswith("--") and bool(text) and (text[0] in "$(!-+.0123456789" or "(" in text)


def bind(event, scope, names):
    keys = names.split(",")
    if any(not identifier(k) or k in scope or k in event.fields for k in keys) or len(set(keys)) != len(keys):
        raise ValueError("invalid or duplicate output name")
    if len(event.fields) + len(keys) > 16:
        raise ValueError("event field limit is 16")
    if len(keys) > 1 and (not isinstance(event.value, list) or len(event.value) != len(keys)):
        raise ValueError("output count mismatch")
    event.fields.update(zip(keys, [event.value] if len(keys) == 1 else event.value))


def milliseconds(value):
    if not isinstance(value, (int, float)) or not 1 <= value <= 2147483647 or int(value) != value:
        raise ValueError("duration/interval requires 1..2147483647 integer milliseconds")
    return int(value)


@dataclass
class Task:
    id: int
    generator: Generator
    wake: int = 0
    writes: set[int] = field(default_factory=set)
    states: dict[int, int] = field(default_factory=dict)
    repeating: bool = False
    legacy: bool = False
    line: int = 1


class ScriptRuntime:
    def __init__(self, hardware=None, jobs=None):
        self.hardware = hardware or HardwareState()
        self.jobs = jobs or JobManager()
        self.functions = {}
        self.globals = {}
        self.tasks: dict[int, Task] = {}
        self.output = []
        self.now = 0
        self.operations = 0
        self.repeat_count = 1
        self.error_line = 1
        self.hardware._script_runtime = self

    def _start(self, generator, label="script", repeating=False, legacy=False):
        if not legacy and sum(not task.legacy for task in self.tasks.values()) >= 8:
            raise ValueError("script job limit is 8")
        if legacy and sum(task.legacy for task in self.tasks.values()) >= 32:
            raise ValueError("schedule queue limit is 32")
        job = self.jobs.create(label)
        task = Task(job.id, generator, self.now, repeating=repeating, legacy=legacy)
        self.tasks[job.id] = task
        return task

    def _finish(self, task):
        task.generator.close()
        for pin, owner in list(self.hardware.pin_owners.items()):
            if owner == task.id:
                if pin in task.writes:
                    self.hardware.set_gpio(pin, 0)
                    self.hardware.script_writes.append((self.now, pin, 0))
                self.hardware.release_pin(pin, task.id)
        self.jobs.kill(task.id)
        self.tasks.pop(task.id, None)

    def kill(self, job_id):
        if job_id not in self.tasks:
            return False
        self._finish(self.tasks[job_id])
        return True

    def reset(self):
        for task in list(self.tasks.values()):
            self._finish(task)
        self.globals.clear()

    def submit(self, source):
        self.error_line = 1
        try:
            self._submit(source)
        except (ValueError, TypeError) as exc:
            raise ValueError(located(str(exc), self.error_line)) from exc

    def _submit(self, source):
        body = parse_script(source)
        definitions = dict(self.functions)
        allowed = {"do", "func", "call", "var", "set", "every", "each", "split", "sleep", "tone", "clock", "adc", "read", "write", "map", "print", "toggle", "not", "lcd", "oled", "ohm", "schedule"}

        def validate(body):
            for pipe in body:
                for stage in pipe:
                    self.error_line = stage.line
                    n, args = stage.name, stage.args
                    if n not in allowed:
                        raise ValueError(f"unsupported script command '{n}'")
                    if n == "schedule":
                        if len(pipe) != 1 or args or not stage.body:
                            raise ValueError("schedule requires entries")
                        continue
                    if n == "func":
                        if len(pipe) != 1 or not args or not stage.body or not all(identifier(t.text) for t in args):
                            raise ValueError("func requires NAME [PARAM...] { BODY }")
                        if len(set(t.text for t in args[1:])) != len(args) - 1:
                            raise ValueError("duplicate function parameter")
                        definitions[args[0].text] = stage
                    if n in {"var", "set"} and (len(args) < 2 or not identifier(args[0].text)):
                        raise ValueError(f"{n} requires NAME EXPR")
                    if n in {"var", "set"}:
                        if len(args) > 2 or expression_token(args[1]):
                            expression(" ".join(t.text for t in args[1:]), {}, Event(), syntax_only=True)
                    else:
                        for token in args:
                            if expression_token(token):
                                expression(token.text, {}, Event(), syntax_only=True)
                    if n == "do" and (len(pipe) != 1 or args or not stage.body):
                        raise ValueError("do requires { BODY }")
                    if n == "every" and (stage is not pipe[0] or len(args) != 1 or len(pipe) == 1):
                        raise ValueError("every requires INTERVAL | COMMAND")
                    if stage.body and n not in {"func", "do", "each"}:
                        raise ValueError("unexpected command body")
                    plain = list(args)
                    for i, token in enumerate(plain):
                        if not token.quoted and token.text == "--out":
                            if i + 2 != len(plain):
                                raise ValueError("--out requires names at end of command")
                            plain = plain[:i]
                            break
                    count = len(plain)
                    valid = True
                    if n in {"each", "toggle", "not", "ohm"}:
                        valid = count == 0
                    elif n in {"sleep", "read"}:
                        valid = count == 1
                    elif n == "adc":
                        valid = count == 1 or count == 2 and plain[1].text == "--watch"
                    elif n == "print":
                        valid = count <= 1
                    elif n in {"write", "lcd"}:
                        valid = count in (1, 2)
                    elif n == "oled":
                        valid = count >= 1
                    elif n == "split":
                        valid = (count == 1 or count == 2 and (plain[1].quoted or plain[1].text != "--value")
                                 or count == 3 and not plain[1].quoted and plain[1].text == "--value")
                    elif n == "map":
                        valid = count == 4 or count == 6 and plain[4].text == "--value"
                    elif n == "tone":
                        valid = count == 5 and plain[1].text == "--hz" and plain[3].text == "--dur"
                    if not valid:
                        raise ValueError(f"invalid arguments for '{n}'")
                    validate(stage.body)
        validate(body)
        if len(definitions) > 16:
            raise ValueError("function limit is 16")

        def calls(body):
            for pipe in body:
                for stage in pipe:
                    self.error_line = stage.line
                    if stage.name == "schedule":
                        continue
                    if stage.name == "call":
                        args = stage.args
                        argc = len(args) - 1 - (2 if len(args) > 2 and args[-2].text == "--out" else 0)
                        if not args or args[0].text not in definitions or argc != len(definitions[args[0].text].args) - 1:
                            raise ValueError("unknown function or incorrect argument count")
                    calls(stage.body)
        calls(body)
        self.functions = definitions
        if any(p[0].name != "func" for p in body):
            self._start(self._body(body, self.globals, Event(timestamp=self.now), 0, fresh=True))

    def _body(self, body, scope, event, depth, fresh=False):
        for pipe in body:
            yield from self._pipe(pipe, scope, Event(timestamp=self.now) if fresh else event, depth)

    def _pipe(self, pipe, scope, event, depth):
        if not pipe:
            return
        self.current.line = pipe[0].line
        self.operations += 1
        if self.operations > 10000:
            raise ValueError("simulation operation limit exceeded")
        stage, tail = pipe[0], pipe[1:]
        n, args = stage.name, list(stage.args)
        task = self.current
        out = None
        for i, token in enumerate(args):
            if not token.quoted and token.text == "--out":
                if i + 2 != len(args):
                    raise ValueError("--out requires names at end of command")
                out, args = args[i+1].text, args[:i]
                break
        if n == "func":
            return
        if n == "do":
            yield from self._body(stage.body, dict(scope), event, depth, fresh=True)
            return
        if n == "clock" or n == "adc" and len(args) == 2 and args[1].text == "--watch":
            if out:
                raise ValueError("use every for named background readings")
            values = [evaluate(t, scope, event) for t in args]
            if n == "clock":
                frequency, duration, fallback_pin = _clock_options([display(v) for v in values])
                if fallback_pin is not None:
                    raise ValueError("use clock HZ | write PIN inside scripts")
                interval = max(1, int(500000 / frequency + 0.5)) / 1000
            else:
                interval, duration = 100, None
            def source():
                started = self.now
                end = started + duration if duration is not None else math.inf
                previous = None
                index = 0
                while duration is not None or index < self.repeat_count:
                    when = started + (index + (n == "clock")) * interval
                    if when >= end:
                        break
                    if when > self.now:
                        yield when - self.now
                    value = (index + 1) % 2 if n == "clock" else self.hardware.read_analog("A0")
                    if n == "clock" or value != previous:
                        yield from self._pipe(tail, scope, Event(value, timestamp=self.now), depth)
                    previous = value
                    index += 1
                if duration is not None and self.now < end:
                    yield end - self.now
            self._start(source(), n, legacy=True)
            return
        if n in {"var", "set"}:
            key = args[0].text
            if key in event.fields or (n == "set" and key not in scope):
                raise ValueError("invalid variable declaration/assignment")
            if key not in scope and len(scope) >= 32:
                raise ValueError("variable limit is 32")
            scope[key] = evaluate(args[1], scope, event) if len(args) == 2 else expression(" ".join(t.text for t in args[1:]), scope, event)
        elif n == "every":
            interval = milliseconds(evaluate(args[0], scope, event))
            def repeat():
                for _ in range(self.repeat_count):
                    started = self.now
                    yield from self._pipe(tail, scope, Event(0, timestamp=self.now), depth)
                    if self.now < started + interval:
                        yield started + interval - self.now
            self._start(repeat(), f"every {interval}", repeating=True)
            return
        elif n == "call":
            if depth >= 8:
                raise ValueError("function call depth exceeds 8")
            fn = self.functions[args[0].text]
            local = {param.text: evaluate(value, scope, event) for param, value in zip(fn.args[1:], args[1:])}
            if local.keys() & event.fields.keys():
                raise ValueError("parameter conflicts with event field")
            yield from self._body(fn.body, local, event, depth + 1)
        elif n == "each":
            if args or not isinstance(event.value, list):
                raise ValueError("each requires a list and optional --out NAME")
            for value in event.value:
                item = Event(value, dict(event.fields), event.timestamp)
                if out:
                    bind(item, scope, out)
                if stage.body:
                    yield from self._body(stage.body, scope, item, depth)
                else:
                    yield from self._pipe(tail, scope, item, depth)
            if stage.body:
                yield from self._pipe(tail, scope, event, depth)
            return
        elif n == "schedule":
            text = "schedule {"
            for entry in stage.body:
                if len(entry) != 1 or entry[0].args or len(entry[0].body) != 1:
                    raise ValueError("invalid schedule entry")
                text += " " + display(evaluate(Token(entry[0].name), scope, event)) + " { "
                commands = []
                for cmd in entry[0].body[0]:
                    words = [cmd.name]
                    for token in cmd.args:
                        value = display(evaluate(token, scope, event))
                        if token.quoted:
                            value = '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'
                        words.append(value)
                    commands.append(" ".join(words))
                text += " | ".join(commands) + " }"
            entries = parse_schedule(text + " }")
            origin = self.now
            for entry, original in zip(entries, stage.body):
                commands = parse_script(entry.pipeline)[0]
                for command, source in zip(commands, original[0].body[0]):
                    command.line = source.line
                def scheduled(commands=commands, offset=entry.start_ms):
                    if self.now < origin + offset:
                        yield origin + offset - self.now
                    yield from self._pipe(commands, scope, Event(timestamp=self.now), depth)
                self._start(scheduled(), "scheduled", legacy=True)
        else:
            values = [evaluate(t, scope, event) for t in args]
            if n == "split":
                if len(values) == 1 and not event.has_value:
                    raise ValueError("split: value is required through pipe or explicit parameter --value")
                delimiter = display(values[0])
                if not delimiter:
                    raise ValueError("split delimiter cannot be empty")
                value = values[2] if len(values) == 3 else values[1] if len(values) == 2 else event.value
                event.value = display(value).split(delimiter)
                event.has_value = True
                if len(event.value) > 64:
                    raise ValueError("list limit is 64")
            elif n == "toggle":
                if values:
                    raise ValueError("toggle takes no arguments")
                task.states[id(stage)] = 1 - task.states.get(id(stage), 0)
                event.value = task.states[id(stage)]
                event.has_value = True
            elif n == "sleep":
                if len(values) != 1:
                    raise ValueError("sleep requires MS")
                yield milliseconds(values[0])
            elif n == "tone":
                if len(values) != 5 or values[1] != "--hz" or values[3] != "--dur":
                    raise ValueError("tone requires PIN --hz HZ --dur MS")
                pin, hz, duration = values[0], values[2], milliseconds(values[4])
                self._claim(pin, task)
                if not isinstance(hz, (int, float)) or not 0.01 <= hz <= 1000000:
                    raise ValueError("invalid tone frequency")
                task.writes.add(int(pin))
                self.hardware.tone_history.append((self.now, int(pin), hz, duration))
                yield duration
                self.hardware.set_gpio(pin, 0)
            else:
                self._host(n, values, event, task)
                if n in {"adc", "read", "write", "map", "not", "ohm"}:
                    event.has_value = True
        if out:
            bind(event, scope, out)
        yield from self._pipe(tail, scope, event, depth)

    def _claim(self, pin, task):
        if not isinstance(pin, (int, float)) or pin != int(pin) or int(pin) not in {0, 2, 4, 5, 12, 13, 14, 15, 16}:
            raise ValueError("GPIO is invalid or reserved")
        self.hardware.claim_pin(int(pin), task.id)

    def _host(self, name, args, event, task):
        if name == "oled":
            self.hardware.oled.command(args, event.value, event.has_value, self.hardware)
            return
        value = event.value
        if "--value" in args:
            index = args.index("--value")
            if index + 2 != len(args):
                raise ValueError("--value requires one value at end")
            value, args = args[-1], args[:index]
        if name == "print" and len(args) <= 1:
            self.output.append(args[0] if args else value)
        elif name == "adc" and len(args) == 1 and str(args[0]).lower() == "a0":
            event.value = self.hardware.read_analog("A0")
        elif name == "read" and len(args) == 1:
            self._claim(args[0], task)
            event.value = self.hardware.read_gpio(args[0])
        elif name == "write" and len(args) in (1, 2):
            self._claim(args[0], task)
            pin = int(args[0])
            task.writes.add(pin)
            event.value = int(float(args[1] if len(args) == 2 else value) != 0)
            self.hardware.set_gpio(pin, event.value)
            self.hardware.script_writes.append((self.now, pin, event.value))
        elif name == "map" and len(args) == 4:
            a, b, c, d = map(float, args)
            if a == b:
                raise ValueError("map input range cannot be zero")
            event.value = math.trunc(c + (float(value) - a) * (d - c) / (b - a))
        elif name == "not" and not args:
            event.value = int(not float(value))
        elif name in {"lcd", "ohm"}:
            result = _dispatch(Command(name, [display(arg) for arg in args]), [] if name == "lcd" and len(args) > 1 else [value], self.hardware, self.jobs, task.id)
            if result:
                event.value = result[0]
        else:
            raise ValueError(f"invalid arguments for '{name}'")

    def tick(self, time_ms):
        if time_ms < self.now:
            raise ValueError("virtual time cannot move backwards at line 1")
        self.now = time_ms
        # Finish already-running finite work before queued entries at a shared
        # boundary; a just-started background source runs on the next tick.
        for task in sorted(list(self.tasks.values()), key=lambda t: (t.wake, t.id)):
            if task.wake > self.now:
                continue
            self.current = task
            try:
                delay = next(task.generator)
                task.wake = self.now + delay
            except StopIteration:
                self._finish(task)
            except (ValueError, TypeError, OverflowError) as exc:
                self._finish(task)
                raise ValueError(located(str(exc), task.line)) from exc

    def run(self, count=1):
        if count < 1:
            raise ValueError("count must be positive at line 1")
        self.repeat_count, self.operations = count, 0
        start = len(self.output)
        while self.tasks:
            self.tick(min(task.wake for task in self.tasks.values()))
        return self.output[start:]

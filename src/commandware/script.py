"""Bounded virtual-time counterpart of the firmware's cooperative script runtime."""
from __future__ import annotations

import math
import re
from dataclasses import dataclass, field
from typing import Generator

from .expressions import Expression, SIGNATURES, format_number, numeric
from .core import HardwareState, JobManager, _lcd


@dataclass
class Token:
    text: str
    quoted: bool = False
    line: int = 1
    raw: str | None = None


@dataclass
class Stage:
    name: str
    args: list[Token] = field(default_factory=list)
    body: list[list[Stage]] = field(default_factory=list)
    line: int = 1
    otherwise: list = field(default_factory=list)
    has_body: bool = False
    source: str = ""


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
            raw_start = i - 1
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
                    if c not in "n\"'\\$":
                        raise ValueError(located("unknown string escape", token_line))
                    c = "\n" if c == "n" else c
                text += c
            if i == len(source):
                raise ValueError(located("unterminated string", token_line))
            i += 1
            tokens.append(Token(text, True, token_line, source[raw_start:i]))
        elif c == "|" and i < len(source) and source[i] == "|":
            i += 1
            tokens.append(Token("||", line=token_line))
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
                name = (tokens[pos].raw or tokens[pos].text) if entries else tokens[pos].text
                command_line = tokens[pos].line
                pos += 1
                nodes += 1
                if nodes > 128:
                    raise fail("script exceeds 128 commands")
                stage = Stage(name, line=command_line, source=source)
                while pos < len(tokens) and not any(at(c) for c in ("{", "}", "|", ";", "\n")):
                    stage.args.append(tokens[pos])
                    pos += 1
                if at("{"):
                    pos += 1
                    stage.has_body = True
                    stage.body = body(depth + 1, name.lower() == "schedule")
                    if not at("}"):
                        raise fail("missing closing brace")
                    pos += 1
                if stage.name.lower() == "if":
                    save = pos
                    while at("\n"):
                        pos += 1
                    if pos < len(tokens) and tokens[pos].text.lower() == "else":
                        pos += 1
                        if not at("{"):
                            raise fail("else requires a block")
                        pos += 1
                        stage.otherwise = body(depth + 1)
                        if not at("}"):
                            raise fail("missing closing brace")
                        pos += 1
                    else:
                        pos = save
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
    completion_group: object = field(default=None, repr=False)

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
    return Expression(text, lambda name: lookup(name, scope, event), notehz).run(syntax_only)


def display(value):
    return format_number(value) if isinstance(value, (float, int)) else str(value)


class Option(str):
    option = True


def evaluate(token, scope, event):
    text = token.text
    if token.quoted and token.raw is not None:
        return expression(token.raw, scope, event)
    if token.quoted:
        return re.sub(r"\$([A-Za-z_][A-Za-z_0-9]*)", lambda m: display(lookup(m[1], scope, event)), text)
    if expression_token(token):
        return expression(text, scope, event)
    return Option(text) if text.startswith("--") else text


def expression_token(token):
    text = token.text
    return not token.quoted and not text.startswith("--") and bool(text) and (text[0] in "\"'$(!-+.0123456789" or "(" in text or text.split()[0].lower() in SIGNATURES)


def argument_source(tokens):
    def quote(text):
        return '"' + text.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'
    return " ".join((t.raw or quote(t.text)) if t.quoted else t.text for t in tokens)


def normalize_arguments(stage):
    if stage.name in {"func", "schedule", "help"}:
        return
    args = stage.args
    if stage.name in SIGNATURES:
        out = next((i for i, t in enumerate(args) if not t.quoted and t.text == "--out"), len(args))
        text = stage.name + " " + argument_source(args[:out])
        expression(text, {}, Event(), syntax_only=True)
        stage.args = [Token(text, line=stage.line)] + args[out:]
        return
    source = argument_source(args)
    normalized = []
    pos = 0
    while pos < len(source):
        while pos < len(source) and source[pos].isspace():
            pos += 1
        if pos == len(source):
            break
        word = re.match(r"[^\s()]+", source[pos:])
        word = word[0] if word else ""
        name_argument = (not normalized and stage.name == "set"
                         or normalized and normalized[-1].text == "--out")
        starts_expression = (source[pos] in "\"'$(!-+.0123456789" and not word.startswith("--")
                             or word.lower() in SIGNATURES)
        if starts_expression and not name_argument:
            parser = Expression(source[pos:])
            parser.parse()
            end = pos + parser.pos
            normalized.append(Token(source[pos:end].strip(), line=stage.line))
            pos = end
        else:
            end = pos
            while end < len(source) and not source[end].isspace():
                end += 1
            normalized.append(Token(source[pos:end], line=stage.line))
            pos = end
    stage.args = normalized


def bind(event, scope, names):
    keys = names.split(",")
    if any(not identifier(k) or k in scope  for k in keys) or len(set(keys)) != len(keys):
        raise ValueError("invalid or duplicate output name")
    if len(set(event.fields) | set(keys)) > 16:
        raise ValueError("event field limit is 16")
    if len(keys) > 1 and (not isinstance(event.value, list) or len(event.value) != len(keys)):
        raise ValueError("output count mismatch")
    event.fields.update(zip(keys, [event.value] if len(keys) == 1 else event.value))


def milliseconds(value):
    if not isinstance(value, (int, float)) or not 1 <= value <= 2147483647 or int(value) != value:
        raise ValueError("duration/interval requires 1..2147483647 integer milliseconds")
    return int(value)


@dataclass
class Pending:
    deadline: float
    callback: object
    event: Event
    roots: list = field(default_factory=list)
    escape: bool = True
    nodes: list = field(default_factory=list)
    completion_group: object = None


@dataclass
class Task:
    id: int
    generator: Generator
    wake: float = 0
    states: dict = field(default_factory=dict)
    repeating: bool = False
    legacy: bool = False
    line: int = 1
    function: str = ""
    pending: dict = field(default_factory=dict)
    callback: object = None
    callback_wake: float = 0
    done: bool = False
    roots: list = field(default_factory=list)
    programs: list = field(default_factory=list)
    frames: int = 0
    busy: int = 0
    nodes: list = field(default_factory=list)
    callback_nodes: list = field(default_factory=list)


class Scope:
    """References see live globals; parameters are immutable captured bindings."""
    def __init__(self, runtime, params=None):
        self.runtime = runtime
        self.params = params or {}

    def __contains__(self, name):
        return name in self.params or name in self.runtime.globals

    def __getitem__(self, name):
        if name in self.params and name in self.runtime.globals:
            raise ValueError(f"ambiguous name '{name}'")
        return self.params[name] if name in self.params else self.runtime.globals[name]


def copy_event(event, timestamp=None):
    return Event(event.value if event.has_value else _UNSET, dict(event.fields),
                 event.timestamp if timestamp is None else timestamp, completion_group=event.completion_group)


def bounded_value(value):
    if isinstance(value, str):
        if len(value.encode('utf-8')) > 4096:
            raise ValueError("string exceeds 4096 bytes")
        return len(value.encode('utf-8'))
    if isinstance(value, list):
        if len(value) > 64:
            raise ValueError("list limit is 64")
        return sum(bounded_value(v) for v in value)
    numeric(value)
    return 8


BUILTINS = {"do", "func", "set", "every", "each", "split", "sleep", "tone", "clock",
            "adc", "read", "write", "map", "print", "toggle", "not", "lcd", "oled",
            "ohm", "schedule", "if", "where", "edge", "rising", "falling", "divide",
            "debounce", "pulse", "shiftin", "shiftout", "help", "pins", "jobs", "kill",
            "status", "reset", "run", "script"}
RESULTS = {"set", "split", "adc", "read", "map", "toggle", "not", "ohm", "shiftin",
           "divide", "pulse", "every", "clock", "script"}


class CancelJob(Exception):
    pass


class ScriptRuntime:
    def __init__(self, hardware=None, jobs=None):
        self.hardware = hardware or HardwareState()
        self.jobs = jobs or JobManager()
        self.functions = {}
        self.saved_scripts = {}  # Simulated device storage; survives runtime reset, not process exit.
        self.globals = {}
        self.tasks = {}
        self.output = []
        self.failures = []
        self.warnings = []
        self.now = 0
        self.operations = 0
        self.repeat_count = 1
        self.error_line = 1
        self.hardware._script_runtime = self

    def _frame_count(self):
        return sum(task.frames + (1 if not task.done and task.frames == 0 else 0)
                   + sum(max(1, sum(isinstance(node, list) for node in pending.nodes)) for pending in task.pending.values())
                   for task in self.tasks.values())

    def _start(self, generator, label="script", repeating=False, legacy=False, roots=None, programs=None, nodes=None):
        if sum(not t.legacy for t in self.tasks.values()) >= 8 and not legacy:
            raise ValueError("script job limit is 8")
        if sum(t.legacy for t in self.tasks.values()) >= 32 and legacy:
            raise ValueError("schedule queue limit is 32")
        if self._frame_count() + 1 > 128:
            raise ValueError("pending continuation frame limit is 128")
        job = self.jobs.create(label)
        task = Task(job.id, generator, self.now, repeating=repeating, legacy=legacy)
        task.roots = roots or []
        task.programs = programs or []
        task.nodes = nodes or []
        self.tasks[job.id] = task
        return task

    def _finish(self, task):
        task.generator.close()
        if task.callback is not None:
            task.callback.close()
        for pin, owner in list(self.hardware.pin_owners.items()):
            if owner == task.id:
                self.hardware.release_pin(pin, task.id)
        self.jobs.kill(task.id)
        self.tasks.pop(task.id, None)

    def kill(self, job_id):
        task = self.tasks.get(job_id)
        if task is None:
            return False
        self._finish(task)
        return True

    def reset(self, cancel_current=True):
        for task in list(self.tasks.values()):
            if cancel_current or task is not getattr(self, 'current', None):
                self._finish(task)
        self.globals.clear()
        self.failures.clear()

    def _memory(self, value=None, key=None, extra=None):
        seen = set()
        def charge(item):
            if isinstance(item, (dict, Event)):
                if id(item) in seen:
                    return 0
                seen.add(id(item))
            if isinstance(item, dict):
                return sum(len(k.encode('utf-8')) + charge(v) for k, v in item.items())
            if isinstance(item, Event):
                return charge(item.value) + charge(item.fields) if item.has_value else charge(item.fields)
            if isinstance(item, list):
                if len(item) > 64:
                    raise ValueError("list limit is 64")
                return sum(charge(v) for v in item)
            return bounded_value(item)
        values = dict(self.globals)
        if key is not None:
            values[key] = value
        total = charge(values)
        for task in self.tasks.values():
            total += sum(charge(root) for root in task.roots)
            total += sum(charge(entry.event) + sum(charge(root) for root in entry.roots) for entry in task.pending.values())
        if key is None and value is not None:
            total += charge(value)
        if extra is not None:
            total += charge(extra)
        if total > 8192:
            raise ValueError("retained value storage exceeds 8192 bytes")

    def _program_memory(self, definitions, source=None):
        programs = {id(fn.source): fn.source for fn in definitions.values()}
        seen = set()
        def visit(root):
            if isinstance(root, Stage):
                if id(root) in seen:
                    return
                seen.add(id(root))
                programs[id(root.source)] = root.source
                visit(root.body)
                visit(root.otherwise)
            elif isinstance(root, list):
                for item in root:
                    visit(item)
        for task in self.tasks.values():
            programs.update({id(program): program for program in task.programs})
            visit(task.nodes)
            visit(task.callback_nodes)
            for pending in task.pending.values():
                visit(pending.nodes)
        if source is not None:
            programs[id(source)] = source
        if sum(len(program.encode()) for program in programs.values()) > 16384:
            raise ValueError("retained program storage exceeds 16384 bytes")

    def _ast_memory(self, definitions=None, extra=None):
        seen = set()
        def visit(root):
            if isinstance(root, Stage):
                if id(root) in seen:
                    return
                seen.add(id(root))
                visit(root.body)
                visit(root.otherwise)
            elif isinstance(root, list):
                for item in root:
                    visit(item)
        for fn in (self.functions if definitions is None else definitions).values():
            visit(fn)
        for task in self.tasks.values():
            visit(task.nodes)
            visit(task.callback_nodes)
            for pending in task.pending.values():
                visit(pending.nodes)
        visit(extra or [])
        if len(seen) > 128:
            raise ValueError("retained command limit is 128")

    def submit(self, source, events=None):
        self.error_line = 1
        try:
            executable, prepared_definitions = self._prepare(source)
            del prepared_definitions
            definitions = None  # Execution resolves new calls against the live table.
            if executable:
                def submission():
                    states = {}
                    for event in events if events is not None else [Event(timestamp=self.now)]:
                        yield from self._body(executable, Scope(self), event, 0, states, definitions)
                roots = [event for event in events] if events is not None else []
                self._memory(extra=roots)
                self._start(submission(), roots=roots, programs=[source], nodes=[executable])
        except (ValueError, TypeError) as exc:
            raise ValueError(located(str(exc), self.error_line)) from exc

    def _prepare(self, source):
        body = parse_script(source)
        definitions = dict(self.functions)
        declared = set()
        for pipe in body:
            for stage in pipe:
                if stage.name.lower() == "func":
                    self.error_line = stage.line
                    if len(pipe) != 1 or not stage.has_body or not stage.args or not all(identifier(t.text) and not t.quoted for t in stage.args):
                        raise ValueError("func requires NAME [PARAM...] { BODY }")
                    name = stage.args[0].text
                    if name.lower() in BUILTINS | set(SIGNATURES):
                        raise ValueError("function name conflicts with built-in command")
                    if name in declared:
                        raise ValueError("duplicate function definition")
                    if len(set(t.text for t in stage.args[1:])) != len(stage.args) - 1:
                        raise ValueError("duplicate function parameter")
                    declared.add(name)
                    stage.name = "func"
                    definitions[name] = stage
        if len(definitions) > 16:
            raise ValueError("function limit is 16")
        self._validate(body, definitions, top=True)
        # Shared submission text remains charged while any definition, call,
        # source, scheduled body, or timer retains its program.
        executable = [p for p in body if p[0].name != "func"]
        self._program_memory(definitions, source if executable else None)
        self._ast_memory(definitions, [executable])
        self.functions = definitions
        return executable, definitions

    def _validate(self, body, definitions, top=False):
        for pipe in body:
            repeating = False
            for stage in pipe:
                self.error_line = stage.line
                lower = stage.name.lower()
                if lower in BUILTINS or lower in SIGNATURES:
                    stage.name = lower
                elif stage.name not in definitions:
                    raise ValueError(f"unsupported command '{stage.name}'")
                n = stage.name
                if n == 'func':
                    if not top or len(pipe) != 1:
                        raise ValueError("function definitions are top-level only")
                    self._validate(stage.body, definitions)
                    continue
                if n == 'schedule':
                    if stage.args or not stage.body:
                        raise ValueError("schedule requires entries")
                    for entry in stage.body:
                        if len(entry) != 1 or not entry[0].has_body:
                            raise ValueError("invalid schedule entry")
                        entry[0].name += (' ' + argument_source(entry[0].args)) if entry[0].args else ''
                        entry[0].args = []
                        expression(entry[0].name, {}, Event(), syntax_only=True)
                        self._validate(entry[0].body, definitions)
                    continue
                normalize_arguments(stage)
                args = stage.args
                out = None
                if any(t.text == '--out' and not t.quoted for t in args):
                    indexes = [i for i, t in enumerate(args) if t.text == '--out' and not t.quoted]
                    if len(indexes) != 1 or indexes[0] != len(args) - 2:
                        raise ValueError("--out requires names at end of command")
                    out, args = args[-1].text, args[:-2]
                    if n not in RESULTS | set(SIGNATURES) | set(definitions) | {'each'}:
                        raise ValueError(f"{n} does not support --out")
                    names = out.split(',')
                    if len(names) != len(set(names)) or not all(identifier(x) for x in names):
                        raise ValueError("invalid or duplicate output name")
                for index, token in enumerate(args):
                    if expression_token(token) and n != 'help' and not (n == 'set' and index == 0):
                        expression(token.text, {}, Event(), syntax_only=True)
                if stage.body and n not in {'do', 'if', 'each'}:
                    raise ValueError("unexpected command body")
                count = len(args)
                valid = True
                if n in definitions:
                    self._function_args(definitions[n], args, syntax_only=True)
                elif n in SIGNATURES:
                    valid = count == 1
                elif n == 'set':
                    valid = count == 2 and identifier(args[0].text)
                elif n == 'do':
                    valid = count == 0 and stage.has_body
                elif n in {'if', 'where'}:
                    valid = count == 1 and (stage.has_body if n == 'if' else not stage.has_body)
                elif n == 'each':
                    valid = count == 0 and stage.has_body
                elif n in {'every', 'sleep', 'read', 'kill', 'run'}:
                    valid = count == 1
                elif n == 'clock':
                    valid = count == 1 or count == 3 and args[1].text == '--dur'
                elif n == 'adc':
                    valid = count == 1 and args[0].text.upper() == 'A0' or count == 2 and args[0].text.upper() == 'A0' and args[1].text == '--watch'
                elif n in {'edge', 'rising', 'falling', 'toggle', 'pins', 'jobs', 'status', 'reset'}:
                    valid = count == 0
                elif n in {'divide', 'debounce', 'pulse'}:
                    valid = count == 1
                elif n == 'tone':
                    valid = count == 5 and args[1].text == '--hz' and args[3].text == '--dur'
                elif n == 'map':
                    valid = count in (4, 5) or count == 6 and args[4].text == '--value'
                elif n in {'print', 'write', 'split'}:
                    base = 0 if n == 'print' else 1
                    valid = count in (base, base + 1) or count == base + 2 and args[base].text == '--value'
                elif n in {'not', 'ohm'}:
                    valid = count in (0, 1) or count == 2 and args[0].text == '--value'
                elif n == 'script':
                    action = args[0].text.lower() if args else ''
                    valid = (action == 'list' and count == 1 or
                             action in {'save', 'load', 'delete'} and count == 2)
                    if out and action not in {'list', 'load'}:
                        raise ValueError("--out unsupported for script action")
                elif n == 'help':
                    valid = count <= 1
                elif n in {'shiftin', 'shiftout'}:
                    self._shift_options(n, args, syntax=True)
                elif n == 'lcd':
                    valid = bool(args) and args[0].text.lower() in {'init', 'print', 'clear'}
                    if valid:
                        action = args[0].text.lower()
                        valid = count == 1 if action == 'clear' else count <= 2 if action == 'init' else count <= 2 or count == 3 and args[1].text == '--value'
                elif n == 'oled':
                    self._oled_syntax(args)
                if not valid:
                    raise ValueError(f"invalid arguments for '{n}'")
                source = n in {'every', 'clock'} or n == 'adc' and count == 2
                if source and repeating:
                    self.warnings.append(f"Warning at line {stage.line}: each incoming event starts a new {n} job.")
                repeating |= source
                self._validate(stage.body, definitions)
                self._validate(stage.otherwise, definitions)

    def _function_args(self, fn, args, scope=None, event=None, syntax_only=False):
        names = [p.text for p in fn.args[1:]]
        if args and args[0].text.startswith('--'):
            if len(args) != len(names) * 2:
                raise ValueError("incorrect function argument count")
            supplied = {}
            for i in range(0, len(args), 2):
                key = args[i].text[2:]
                if not args[i].text.startswith('--') or key not in names or key in supplied:
                    raise ValueError("unknown or duplicate function argument")
                supplied[key] = args[i+1]
            tokens = [supplied[name] for name in names]
        else:
            if len(args) != len(names) or any(t.text.startswith('--') for t in args):
                raise ValueError("incorrect function argument count")
            tokens = args
        return {} if syntax_only else dict(zip(names, [evaluate(t, scope, event) for t in tokens]))

    def _body(self, body, scope, event, depth, states, definitions):
        result = [copy_event(event)]
        task = self.current
        for index, pipe in enumerate(body):
            before = {key: pending for key, pending in task.pending.items()}
            statement_event = copy_event(event)
            if index != len(body) - 1:
                statement_event.completion_group = object()
            result = yield from self._pipe(pipe, scope, statement_event, depth, states, definitions)
            if index != len(body) - 1:
                for key, pending in task.pending.items():
                    if before.get(key) is not pending:
                        pending.escape = False
                yield from self._drain(statement_event.completion_group)
        return result

    def _drain(self, group):
        """Complete a finite statement/item without admitting another source event."""
        task = self.current
        task.busy += 1
        try:
            while True:
                pending = [(entry.deadline, key, entry) for key, entry in task.pending.items()
                           if entry.completion_group is group]
                if not pending:
                    return
                deadline, key, entry = min(pending, key=lambda item: (item[0], item[1]))
                if deadline > self.now:
                    yield deadline - self.now
                if task.pending.get(key) is not entry:
                    continue
                del task.pending[key]
                previous_nodes = task.callback_nodes
                task.callback_nodes = previous_nodes + entry.nodes
                task.roots.extend(entry.roots + [entry.event])
                try:
                    yield from entry.callback()
                finally:
                    task.callback_nodes = previous_nodes
                    for root in reversed(entry.roots + [entry.event]):
                        for index in range(len(task.roots) - 1, -1, -1):
                            if task.roots[index] is root:
                                del task.roots[index]
                                break
        finally:
            task.busy -= 1

    def _pipe(self, pipe, scope, event, depth, states, definitions):
        if not pipe:
            return [event]
        task = self.current
        if self._frame_count() >= 128:
            raise ValueError("pending continuation frame limit is 128")
        task.frames += 1
        task.roots.append(event)
        try:
            return (yield from self._pipe_impl(pipe, scope, event, depth, states, definitions))
        finally:
            task.frames -= 1
            for index in range(len(task.roots) - 1, -1, -1):
                if task.roots[index] is event:
                    del task.roots[index]
                    break

    def _pipe_impl(self, pipe, scope, event, depth, states, definitions):
        if not pipe:
            return [event]
        stage, tail = pipe[0], pipe[1:]
        task = self.current
        task.line = stage.line
        self.operations += 1
        if self.operations > 100000:
            raise ValueError("simulation operation limit exceeded")
        n, args = stage.name, list(stage.args)
        out = None
        if len(args) >= 2 and args[-2].text == '--out' and not args[-2].quoted:
            out, args = args[-1].text, args[:-2]
        if n in {'every', 'clock'} or n == 'adc' and len(args) == 2:
            values = [evaluate(t, scope, event) for t in args]
            interval, duration = 100, None
            if n == 'every':
                interval = milliseconds(values[0])
            elif n == 'clock':
                hz = numeric(values[0])
                if not .01 <= hz <= 1000000:
                    raise ValueError("invalid clock frequency")
                interval = max(1, math.floor(500000 / hz + .5)) / 1000
                duration = milliseconds(values[2]) if len(values) == 3 else None
            captured = Scope(self, dict(scope.params))
            origin = self.now
            def source():
                index = 1 if n == 'clock' else 0
                delivered = 0
                previous = _UNSET
                end = origin + duration if duration is not None else math.inf
                source_states = {}
                while duration is not None or delivered < self.repeat_count:
                    deadline = origin + index * interval
                    if deadline >= end:
                        break
                    if deadline > self.now:
                        yield deadline - self.now
                    # First every/ADC observation runs at first service; subsequent
                    # deadlines stay on the registration timeline.
                    if delivered or n == 'clock':
                        index = max(index, math.ceil((self.now - origin) / interval - 1e-10))
                        deadline = origin + index * interval
                        if deadline > self.now:
                            continue
                    if self.now >= end:
                        break
                    value = index % 2 if n == 'clock' else self.hardware.read_analog('A0') if n == 'adc' else 0
                    if n != 'adc' or value != previous:
                        item = Event(value, timestamp=self.now)
                        if out:
                            self._bind(item, captured, out)
                        yield from self._pipe(tail, captured, item, depth, source_states, definitions)
                    previous = value
                    delivered += 1
                    index = max(index + 1, math.ceil((self.now - origin) / interval - 1e-10))
                if duration is not None and self.now < end:
                    yield end - self.now
            self._memory(extra=captured.params)
            self._start(source(), n, repeating=True, roots=[captured.params], programs=[stage.source], nodes=[stage, tail])
            return []
        if n == 'schedule':
            offsets = []
            for entry in stage.body:
                offset = numeric(expression(entry[0].name, scope, event))
                if offset != int(offset) or not 0 <= offset <= 2147483647:
                    raise ValueError("schedule offset requires nonnegative integer milliseconds")
                offsets.append(offset)
            if sum(t.legacy for t in self.tasks.values()) + len(offsets) > 32:
                raise ValueError("schedule queue limit is 32")
            if self._frame_count() + len(offsets) > 128:
                raise ValueError('pending continuation frame limit is 128')
            origin = self.now
            prepared = [(offset, entry, copy_event(event), Scope(self, dict(scope.params))) for offset, entry in zip(offsets, stage.body)]
            self._memory(extra=[value for _, _, ev, sc in prepared for value in [ev, sc.params]])
            for offset, entry, captured_event, captured in prepared:
                def scheduled(offset=offset, entry=entry, captured=captured, captured_event=captured_event):
                    if self.now < origin + offset:
                        yield origin + offset - self.now
                    if sum(not t.legacy for t in self.tasks.values()) >= 8:
                        raise ValueError('script job limit is 8')
                    self.current.legacy = False
                    yield from self._body(entry[0].body, captured, copy_event(captured_event, self.now), depth, {}, definitions)
                self._memory(extra=[captured_event, captured.params])
                self._start(scheduled(), 'scheduled', legacy=True, roots=[captured_event, captured.params], programs=[stage.source], nodes=[entry[0].body])
            return []
        pending_before = dict(task.pending)
        if n == 'do':
            result = yield from self._body(stage.body, scope, event, depth, states, definitions)
        elif n == 'if':
            selected = stage.body if numeric(evaluate(args[0], scope, event)) != 0 else stage.otherwise
            result = yield from self._body(selected, scope, event, depth, states, definitions)
        elif n == 'where':
            result = [event] if numeric(evaluate(args[0], scope, event)) != 0 else []
        elif n in self.functions:
            fn = self.functions[n]
            if depth >= 8:
                raise ValueError("function call depth exceeds 8")
            params = self._function_args(fn, args, scope, event)
            if set(params) & (set(self.globals) | set(event.fields)):
                raise ValueError("parameter conflicts with variable or event field")
            self._memory(extra=params)
            task.roots.append(params)
            task.programs.append(fn.source)
            task.nodes.append(fn)
            previous_function = task.function
            task.function = n
            try:
                result = yield from self._body(fn.body, Scope(self, params), event, depth + 1, {}, definitions)
            except ValueError as exc:
                raise ValueError(f"{exc} in function {n}") from exc
            finally:
                task.function = previous_function
                task.roots.remove(params)
                task.programs.remove(fn.source)
                task.nodes.remove(fn)
        elif n == 'each':
            if not event.has_value or not isinstance(event.value, list):
                raise ValueError("each requires a list")
            for value in event.value:
                item = Event(value, dict(event.fields), event.timestamp, completion_group=object())
                if out:
                    self._bind(item, scope, out)
                before_item = dict(task.pending)
                yield from self._body(stage.body, scope, item, depth, states, definitions)
                for key, pending in task.pending.items():
                    if before_item.get(key) is not pending:
                        pending.escape = False
                yield from self._drain(item.completion_group)
            out = None
            result = [event]
        else:
            values = [t.text for t in args] if n == 'help' else [args[0].text, evaluate(args[1], scope, event)] if n == 'set' else [evaluate(t, scope, event) for t in args]
            for value in values:
                bounded_value(value)
            if n == 'set':
                name, value = args[0].text, values[1]
                if name in scope.params or name in event.fields:
                    raise ValueError("assignment conflicts with parameter or event field")
                if name not in self.globals and len(self.globals) >= 32:
                    raise ValueError("variable limit is 32")
                old_value, old_has_value = event.value, event.has_value
                event.value, event.has_value = value, True
                try:
                    self._memory(value, name)
                finally:
                    event.value, event.has_value = old_value, old_has_value
                self.globals[name] = value
                event.value, event.has_value = value, True
            elif n in SIGNATURES:
                event.value, event.has_value = values[0], True
            elif n == 'sleep':
                task.busy += 1
                try:
                    yield self._duration(values[0])
                finally:
                    task.busy -= 1
            elif n == 'tone':
                pin, hz, duration = values[0], numeric(values[2]), self._duration(values[4])
                pin = self._claim(pin, task)
                if not .01 <= hz <= 1000000:
                    raise ValueError("invalid tone frequency")
                self.hardware.tone_history.append((self.now, int(pin), hz, duration))
                # Last physical half-cycle level; completion does not clean up GPIO.
                if duration:
                    self.hardware.set_gpio(pin, 0)
                task.busy += 1
                try:
                    yield duration
                finally:
                    task.busy -= 1
                if duration:
                    half_period_us = max(1, math.floor(500000 / hz + .5))
                    transitions = max(0, math.ceil(duration * 1000 / half_period_us) - 1)
                    self.hardware.set_gpio(pin, transitions % 2)
            elif n in {'edge', 'rising', 'falling', 'toggle', 'divide', 'debounce', 'pulse'}:
                result = yield from self._signal(n, values, event, stage, scope, tail, depth, states, definitions, out)
                if result is None:
                    return []
                if not result:
                    return []
                event = result[0]
            elif n == 'reset':
                self.reset(cancel_current=False)
                raise CancelJob()
            elif n == 'kill':
                target = numeric(values[0])
                if int(target) != target:
                    raise ValueError("kill requires integer job ID")
                if int(target) == task.id:
                    raise CancelJob()
                if not self.kill(int(target)):
                    raise ValueError("unknown job ID")
            elif n == 'run':
                from pathlib import Path
                source = Path(str(values[0])).read_text()
                body, prepared_definitions = self._prepare(source)
                del prepared_definitions
                file_definitions = None
                task.programs.append(source)
                task.nodes.append(body)
                try:
                    result = yield from self._body(body, scope, event, depth, states, file_definitions)
                finally:
                    task.programs.remove(source)
                    task.nodes.remove(body)
                for item in result:
                    yield from self._pipe(tail, scope, item, depth, states, definitions)
                return []
            else:
                self._host(n, values, event, task)
            result = [event]
        if n in {'do', 'if'} or n in self.functions:
            for key, pending in task.pending.items():
                if pending_before.get(key) is pending or not pending.escape:
                    continue
                old_callback = pending.callback
                def composed(old_callback=old_callback):
                    delayed = yield from old_callback()
                    outgoing = []
                    for delayed_event in delayed or []:
                        task.line = stage.line
                        if out:
                            self._bind(delayed_event, scope, out)
                        outgoing.extend((yield from self._pipe(tail, scope, delayed_event, depth, states, definitions)))
                    return outgoing
                pending.callback = composed
                pending.roots.append(scope.params)
                pending.nodes.append(tail)
            if self._frame_count() > 128:
                raise ValueError('pending continuation frame limit is 128')
        outgoing = []
        for item in result:
            if item.has_value:
                bounded_value(item.value)
            self._memory()
            if out:
                if not item.has_value:
                    raise ValueError("output binding requires a value")
                self._bind(item, scope, out)
            outgoing.extend((yield from self._pipe(tail, scope, item, depth, states, definitions)))
        return outgoing

    @staticmethod
    def _duration(value):
        value = numeric(value)
        if value != int(value) or not 0 <= value <= 2147483647:
            raise ValueError("duration requires nonnegative integer milliseconds")
        return int(value)

    def _signal(self, name, args, event, stage, scope, tail, depth, states, definitions, out):
        state = states.setdefault(id(stage), {})
        task = self.current
        if name == 'toggle':
            state['bit'] = 1 - state.get('bit', 0)
            event.value, event.has_value = state['bit'], True
            return [event]
        if name != 'pulse':
            value = numeric(self._input([], event))
            if value not in (0, 1):
                raise ValueError(f"{name} requires signal 0 or 1")
            previous = state.get('previous')
            state['previous'] = value
        if name in {'edge', 'rising', 'falling'}:
            emit = previous is not None and (previous != value if name == 'edge' else previous == (0 if name == 'rising' else 1) and value == (1 if name == 'rising' else 0))
            return [event] if emit else []
        if name == 'divide':
            divisor = milliseconds(numeric(args[0]))
            if previous is None:
                state['count'], state['bit'] = 0, 0
            elif previous == value:
                return []
            else:
                state['count'] += 1
                if state['count'] < divisor:
                    return []
                state['count'] = 0
                state['bit'] = 1 - state['bit']
            event.value, event.has_value = state['bit'], True
            return [event]
        duration = milliseconds(numeric(args[0]))
        key = id(state)
        if key not in task.pending and self._frame_count() + 1 > 128:
            raise ValueError('pending continuation frame limit is 128')
        if name == 'debounce':
            if previous == value:
                return []
            captured = copy_event(event)
            def expired():
                if state.get('accepted', _UNSET) != numeric(captured.value):
                    state['accepted'] = numeric(captured.value)
                    return (yield from self._pipe(tail, scope, captured, depth, states, definitions))
                return []
            task.pending[key] = Pending(self.now + duration, expired, captured, [scope.params], nodes=[stage, tail], completion_group=event.completion_group)
            return []
        captured = Event(0, dict(event.fields), event.timestamp, completion_group=event.completion_group)
        def expired():
            state['active'] = False
            if out:
                self._bind(captured, scope, out)
            return (yield from self._pipe(tail, scope, captured, depth, states, definitions))
        task.pending[key] = Pending(self.now + duration, expired, captured, [scope.params], nodes=[stage, tail], completion_group=event.completion_group)
        if state.get('active'):
            return []
        state['active'] = True
        event.value, event.has_value = 1, True
        return [event]
        yield  # This function is a coroutine even for an immediate signal.

    def _bind(self, event, scope, names):
        previous = event.fields
        event.fields = dict(previous)
        try:
            bind(event, scope, names)
            self._memory(extra=event)
        except Exception:
            event.fields = previous
            raise

    def _claim(self, pin, task):
        pin = numeric(pin)
        if pin != int(pin) or int(pin) not in {0, 2, 4, 5, 12, 13, 14, 15, 16}:
            raise ValueError("GPIO is invalid or reserved")
        if self.hardware.lcd_ready and int(pin) in (4, 5):
            raise ValueError('GPIO4/GPIO5 are reserved for LCD I2C')
        self.hardware.claim_pin(int(pin), task.id)
        return int(pin)

    @staticmethod
    def _input(args, event, base=0):
        rest = args[base:]
        if rest:
            if len(rest) == 1:
                return rest[0]
            if len(rest) == 2 and isinstance(rest[0], Option) and rest[0] == '--value':
                return rest[1]
            raise ValueError("invalid explicit value")
        if not event.has_value:
            raise ValueError("pipeline value is required")
        return event.value

    def _host(self, name, args, event, task):
        if name == 'print':
            value = self._input(args, event)
            self.output.append(value if not isinstance(value, list) else self._list_text(value))
        elif name == 'script':
            action = args[0].lower()
            if action == 'list':
                event.value, event.has_value = sorted(self.saved_scripts), True
                return
            name = args[1]
            if (not isinstance(name, str) or not 1 <= len(name) <= 28 or
                    name != name.strip() or not re.fullmatch(r'[A-Za-z0-9_ -]+', name)):
                raise ValueError("script: name must be 1-28 ASCII letters, digits, spaces, _ or -; no surrounding spaces")
            if action == 'save':
                if not event.has_value or not isinstance(event.value, str):
                    raise ValueError("script save requires a string input")
                bounded_value(event.value)
                if name not in self.saved_scripts and len(self.saved_scripts) >= 32:
                    raise ValueError("script: script limit is 32")
                self.saved_scripts[name] = event.value
            else:
                if name not in self.saved_scripts:
                    raise ValueError("script: script not found")
                if action == 'load':
                    event.value, event.has_value = self.saved_scripts[name], True
                else:
                    del self.saved_scripts[name]
        elif name == 'adc':
            event.value, event.has_value = self.hardware.read_analog('A0'), True
        elif name == 'read':
            pin = self._claim(args[0], task)
            event.value, event.has_value = self.hardware.read_gpio(pin), True
        elif name == 'write':
            value = numeric(self._input(args, event, 1))
            pin, bit = self._claim(args[0], task), int(value != 0)
            self.hardware.set_gpio(pin, bit)
            self.hardware.script_writes.append((self.now, pin, bit))
        elif name == 'map':
            a, b, c, d = map(numeric, args[:4])
            if a == b:
                raise ValueError("map input range cannot be zero")
            value = c + (numeric(self._input(args, event, 4)) - a) * (d - c) / (b - a)
            event.value, event.has_value = math.trunc(numeric(value)), True
        elif name == 'split':
            delimiter, value = args[0], self._input(args, event, 1)
            if not isinstance(delimiter, str) or not delimiter or not isinstance(value, str):
                raise ValueError("split requires text and a nonempty delimiter")
            event.value, event.has_value = value.split(delimiter), True
            bounded_value(event.value)
        elif name == 'not':
            event.value, event.has_value = int(numeric(self._input(args, event)) == 0), True
        elif name == 'oled':
            self.hardware.oled.command(args, event.value, event.has_value, self.hardware, strict_options=True)
        elif name == 'ohm':
            from .core import _ohm_reading
            value = numeric(self._input(args, event))
            if int(value) != value:
                raise ValueError('ohm requires integer ADC input')
            event.value, event.has_value = _ohm_reading(int(value)), True
        elif name in {'shiftin', 'shiftout'}:
            self._shift(name, args, event, task)
        elif name == 'lcd':
            action = str(args[0]).lower()
            if action == 'print':
                value = self._input(args, event, 1)
                text = self._list_text(value) if isinstance(value, list) else display(value)
                _lcd(['print', text], self.hardware, [])
            else:
                if action == 'init' and any(pin in self.hardware.pin_owners for pin in (4, 5)):
                    raise ValueError('LCD I2C pins are owned by a job')
                _lcd([display(v) for v in args], self.hardware, [])
        elif name == 'jobs':
            self.output.extend(self.jobs.list_jobs() + [f"{job} FAILED {message}" for job, message in self.failures])
        elif name == 'pins':
            self.output.extend(self.hardware.pins_snapshot())
        elif name == 'status':
            self.output.append(f"jobs={len(self.tasks)} globals={len(self.globals)} functions={len(self.functions)}")
        elif name == 'help':
            self.output.append('Commands: ' + ' '.join(sorted(BUILTINS | set(SIGNATURES))))
        else:
            raise ValueError(f"invalid arguments for '{name}'")

    @staticmethod
    def _oled_syntax(args):
        if not args or args[0].text.lower() not in {'init', 'print', 'chart', 'rect', 'clear'}:
            raise ValueError("oled requires init, print, chart, rect, or clear")
        action = args[0].text.lower()
        allowed = {'init': {'--address', '--width', '--height'}, 'print': {'--x', '--y', '--value'},
                   'chart': {'--x', '--y', '--width', '--height', '--min', '--max', '--value'}, 'rect': {'--value'}, 'clear': set()}[action]
        positional, flags = [], set()
        i = 1
        while i < len(args):
            token = args[i]
            if token.text.startswith('--'):
                if token.text not in allowed or token.text in flags or i + 1 == len(args):
                    raise ValueError("invalid OLED options")
                flags.add(token.text)
                i += 2
            else:
                positional.append(token)
                i += 1
        if action == 'clear' and positional or action == 'init' and len(positional) > 1:
            raise ValueError("invalid OLED arguments")
        if action == 'rect' and len(positional) != 4:
            raise ValueError('oled rect requires X Y WIDTH HEIGHT')
        if action in {'print', 'chart'}:
            if len(positional) > 3:
                raise ValueError("invalid OLED arguments")
            if len(positional) >= 2 and flags & {'--x', '--y'}:
                raise ValueError("OLED coordinate arguments conflict")
            if len(positional) in (1, 3) and '--value' in flags:
                raise ValueError("duplicate OLED value")

    @staticmethod
    def _shift_options(name, args, syntax=False):
        def text(value):
            return value.text if syntax else str(value)
        def option(value):
            return text(value).startswith('--') and (not value.quoted if syntax else isinstance(value, Option))
        required = 3 if name == 'shiftin' else 2
        if len(args) < required:
            raise ValueError(f"{name} requires hardware pins")
        pins, order, bits, value = args[:required], 'msb', None, _UNSET
        order_set = False
        i = required
        while i < len(args):
            token = text(args[i])
            if option(args[i]):
                if token not in {'--bits', '--value'} or i + 1 == len(args):
                    raise ValueError("invalid shift-register option")
                if token == '--bits':
                    if bits is not None:
                        raise ValueError("duplicate --bits")
                    bits = args[i + 1]
                else:
                    if name != 'shiftout' or value is not _UNSET:
                        raise ValueError("invalid shift-register value")
                    value = args[i + 1]
                i += 2
            elif token.lower() in {'msb', 'lsb'}:
                if order_set:
                    raise ValueError("duplicate bit order")
                order, order_set = token.lower(), True
                i += 1
            elif name == 'shiftout' and len(pins) == 2:
                pins = pins + [args[i]]
                i += 1
            else:
                raise ValueError("invalid shift-register arguments")
        if syntax:
            return
        bits = 8 if bits is None else numeric(bits)
        if bits != int(bits) or not 1 <= bits <= 53:
            raise ValueError("shift-register width requires integer 1..53")
        return pins, order, int(bits), value

    def _shift(self, name, args, event, task):
        pins, order, bits, value = self._shift_options(name, args)
        pins = [self._claim(pin, task) for pin in pins]
        if len(set(pins)) != len(pins):
            raise ValueError("shift-register pins must be distinct")
        data, clock = pins[:2]
        latch = pins[2] if len(pins) == 3 else None
        def drive(pin, level):
            self.hardware.set_gpio(pin, level)
            self.hardware.script_writes.append((self.now, pin, level))
        if name == 'shiftout':
            value = numeric(self._input([], event) if value is _UNSET else value)
            if value != int(value) or not 0 <= value < 2 ** bits:
                raise ValueError("shiftout value exceeds frame range")
            if latch is not None:
                drive(latch, 0)
            for index in range(bits):
                bit = bits - index - 1 if order == 'msb' else index
                drive(clock, 0)
                drive(data, int(value) >> bit & 1)
                drive(clock, 1)
                drive(clock, 0)
            if latch is not None:
                drive(latch, 1)
        else:
            drive(latch, 0)
            drive(latch, 1)
            result = 0
            for index in range(bits):
                drive(clock, 0)
                bit = numeric(self.hardware.read_gpio(data))
                if bit not in (0, 1):
                    raise ValueError("shiftin data requires Boolean input")
                shift = bits - index - 1 if order == 'msb' else index
                result |= int(bit) << shift
                drive(clock, 1)
                drive(clock, 0)
            event.value, event.has_value = result, True

    @staticmethod
    def _list_text(value):
        import json
        return '[' + ', '.join(json.dumps(v, ensure_ascii=False) if isinstance(v, str) else display(v) for v in value) + ']'

    def _wake(self, task):
        if task.callback is not None:
            return task.callback_wake
        if task.busy and not task.done:
            return task.wake
        deadlines = [value.deadline for value in task.pending.values()]
        if not task.done:
            deadlines.append(task.wake)
        return min(deadlines) if deadlines else math.inf

    def tick(self, time_ms):
        if time_ms < self.now:
            raise ValueError("virtual time cannot move backwards at line 1")
        self.now = time_ms
        errors = []
        for task in sorted(list(self.tasks.values()), key=lambda t: (self._wake(t), t.id)):
            if task.id not in self.tasks or self._wake(task) > self.now:
                continue
            self.current = task
            try:
                if task.callback is None and not task.busy:
                    due = [(entry.deadline, key, entry) for key, entry in task.pending.items() if entry.deadline <= self.now]
                    if due:
                        _, key, entry = min(due, key=lambda x: (x[0], x[1]))
                        del task.pending[key]
                        task.callback = entry.callback()
                        task.callback_nodes = entry.nodes
                        task.callback_wake = self.now
                if task.callback is not None:
                    try:
                        task.callback_wake = self.now + next(task.callback)
                    except StopIteration:
                        task.callback = None
                        task.callback_nodes = []
                elif not task.done and task.wake <= self.now:
                    try:
                        task.wake = self.now + next(task.generator)
                    except StopIteration:
                        task.done = True
                if task.done and not task.pending and task.callback is None:
                    self._finish(task)
            except CancelJob:
                self._finish(task)
            except (ValueError, TypeError, OverflowError) as exc:
                message = located(str(exc), task.line)
                self.failures.append((task.id, message))
                self.failures[:] = self.failures[-8:]
                self._finish(task)
                errors.append(message)
        if errors:
            raise ValueError(errors[0])

    def run(self, count=1):
        if count < 1:
            raise ValueError("count must be positive at line 1")
        self.repeat_count, self.operations = count, 0
        start = len(self.output)
        errors = []
        while self.tasks:
            try:
                self.tick(max(self.now, min(self._wake(task) for task in self.tasks.values())))
            except ValueError as exc:
                errors.append(str(exc))
        if errors:
            raise ValueError(errors[0])
        return self.output[start:]

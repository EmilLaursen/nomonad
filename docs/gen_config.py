#!/usr/bin/env python3
import sys


class Str(str):
    pass


def parse(text):
    stack = [[]]
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in " \t\n\r":
            i += 1
        elif c == ";":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "(":
            stack.append([])
            i += 1
        elif c == ")":
            done = stack.pop()
            stack[-1].append(done)
            i += 1
        elif c == '"':
            i += 1
            buf = []
            while text[i] != '"':
                if text[i] == "\\":
                    i += 1
                    buf.append({"n": "\n", "t": "\t"}.get(text[i], text[i]))
                else:
                    buf.append(text[i])
                i += 1
            stack[-1].append(Str("".join(buf)))
            i += 1
        else:
            j = i
            while j < n and text[j] not in ' \t\n\r()";':
                j += 1
            stack[-1].append(text[i:j])
            i = j
    return stack[0]


def render(x):
    if isinstance(x, list):
        return "(" + " ".join(render(y) for y in x) + ")"
    if isinstance(x, Str):
        return '"' + x.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return x


def kw(form, key, start=2):
    for i in range(start, len(form) - 1):
        if form[i] == key and not isinstance(form[i], Str):
            return form[i + 1]
    return None


def cell(s):
    return " ".join(s.split("\n")).replace("|", "\\|")


def code(x):
    return "`" + cell(render(x)) + "`"


def key_table(out, keys):
    out.append("| Keyword | Type | Default | Description |")
    out.append("|---|---|---|---|")
    for k in keys:
        if kw(k, ":required") == "true":
            default = "required"
        else:
            d = kw(k, ":default")
            default = code(d) if d is not None else ""
        out.append("| `%s` | %s | %s | %s |" % (
            k[0], code(k[1]), default, cell(kw(k, ":doc"))))
    out.append("")


def entry_usage(pat, keys):
    text = render(pat)
    if keys:
        extra = " ".join("[%s %s]" % (k[0], render(k[1])) for k in keys)
        text = text[:-1] + " " + extra + ")"
    return text


def do_types(out, types):
    out.append("## Types")
    out.append("")
    out.append("| Type | Definition | Description |")
    out.append("|---|---|---|")
    for t in types:
        doc = kw(t, ":doc")
        end = next(i for i, x in enumerate(t)
                   if x == ":doc" and not isinstance(x, Str))
        rest = t[2:end]
        binds = kw(rest, ":types", 0) or []
        if binds:
            rest = rest[:rest.index(":types")]
        definition = " ".join(render(x) for x in rest)
        definition += "".join(
            ", %s is %s" % (b[0], render(b[1])) for b in binds)
        out.append("| `%s` | %s | %s |" % (
            t[1], "`" + definition + "`" if definition else "", cell(doc)))
    out.append("")


def do_forms(out, forms):
    out.append("## Top-level forms")
    out.append("")
    for f in forms:
        name = f[1]
        head = kw(f, ":head") or []
        entries = kw(f, ":entries") or []
        types = kw(f, ":types") or []
        out.append("### %s" % name)
        out.append("")
        out.append(kw(f, ":doc"))
        out.append("")
        out.append("```")
        first = "(" + " ".join([name] + [render(h) for h in head])
        if entries:
            out.append(first)
            for e in entries:
                keys = kw(e, ":keys", 1) or []
                out.append("  " + entry_usage(e[0], keys))
            out.append("  ...)")
        else:
            out.append(first + ")")
        out.append("```")
        out.append("")
        out.append("| Placeholder | Type |")
        out.append("|---|---|")
        for t in types:
            out.append("| `%s` | %s |" % (t[0], code(t[1])))
        out.append("")
        for e in entries:
            keys = kw(e, ":keys", 1)
            if keys:
                out.append("Keywords of `%s`:" % render(e[0]))
                out.append("")
                key_table(out, keys)


def do_defcfg(out, blocks):
    out.append("## defcfg blocks")
    out.append("")
    for b in blocks:
        name = b[1]
        out.append("### %s" % name)
        out.append("")
        out.append(kw(b, ":doc"))
        out.append("")
        keys = kw(b, ":keys")
        args = kw(b, ":args")
        if keys is not None:
            out.append("```")
            out.append("(defcfg (%s %s))" % (name, " ".join(
                ("%s VALUE" if kw(k, ":required") == "true" else
                 "[%s VALUE]") % k[0] for k in keys)))
            out.append("```")
            out.append("")
            key_table(out, keys)
        else:
            usage = " ".join(
                a[0].upper() + (" ..." if isinstance(a[1], list) and
                                a[1][0] == "rest-of" else "")
                for a in args)
            out.append("```")
            out.append("(defcfg (%s %s))" % (name, usage))
            out.append("```")
            out.append("")
            out.append("| Argument | Type | Default |")
            out.append("|---|---|---|")
            for a in args:
                out.append("| `%s` | %s | %s |" % (
                    a[0].upper(), code(a[1]), code(kw(b, ":default"))))
            out.append("")


def do_actions(out, actions):
    out.append("## Actions")
    out.append("")
    out.append("Used as the ACTION of defkeys, defmouse and defsubmap "
               "entries, and by `nomonad -m`.")
    out.append("")
    out.append("| Action | Arguments | Layout | Description |")
    out.append("|---|---|---|---|")
    for a in actions:
        name = a[1]
        args = kw(a, ":args") or []
        layout = kw(a, ":layout")
        if args:
            usage = "(%s %s)" % (name, " ".join(x[0].upper() for x in args))
            argdoc = ", ".join(
                "`%s` %s" % (x[0].upper(), code(x[1])) for x in args)
        else:
            usage = name
            argdoc = ""
        out.append("| `%s` | %s | %s | %s |" % (
            usage, argdoc, layout or "", cell(kw(a, ":doc"))))
    out.append("")


def main():
    top = parse(sys.stdin.read())
    schema = top[0]
    if schema[0] != "nomonad-schema":
        sys.exit("gen_config.py: not a nomonad schema")
    version = schema[1][1]
    groups = {"type": [], "form": [], "defcfg": [], "action": []}
    for e in schema[2:]:
        groups[e[0]].append(e)
    out = [
        "# nomonad config reference",
        "",
        "Generated by `make docs` from `nomonad --describe` (schema "
        "version %s). Do not edit by hand." % version,
        "",
        "The config is `$XDG_CONFIG_HOME/nomonad/config.scm`, a sequence "
        "of the top-level forms below. Upper case words in patterns are "
        "placeholders, `[...]` marks an optional keyword and `...` means "
        "more entries of the same shape.",
        "",
    ]
    do_forms(out, groups["form"])
    do_defcfg(out, groups["defcfg"])
    do_actions(out, groups["action"])
    do_types(out, groups["type"])
    sys.stdout.write("\n".join(out).rstrip("\n") + "\n")


main()

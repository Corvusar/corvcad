#!/usr/bin/env python3
"""Minimal Mattermost Boards CLI for Claude Code (stdlib only).

Env: MM_TOKEN (required, your <user>-claude token), MM_URL (required, the Mattermost server)

  board.py me                          your account (<user>-claude)
  board.py boards                      boards you can see
  board.py cards BOARD [STATUS]        cards on a board, optionally filtered by status
  board.py show CARD                   card properties, description, checklist, comments
  board.py create BOARD TITLE [STATUS] new card (default status Planned), prints its id
  board.py move CARD STATUS           set the card's Status (e.g. "In Progress", "Done")
  board.py assign CARD [USERNAME]      set the card's Assignee (default: you)
  board.py comment CARD TEXT           add a comment to the card
  board.py describe CARD TEXT          append a paragraph (markdown) to the card's description
"""
import json, os, sys, time, urllib.error, urllib.request

URL = (os.environ.get("MM_URL") or sys.exit("MM_URL is not set")).rstrip("/")
TOKEN = os.environ.get("MM_TOKEN") or sys.exit("MM_TOKEN is not set")


def call(method, path, body=None):
    full = URL + (path if path.startswith("/api/") else "/plugins/focalboard/api/v2" + path)
    req = urllib.request.Request(full, method=method, data=None if body is None else json.dumps(body).encode(),
                                 headers={"Authorization": "Bearer " + TOKEN, "X-Requested-With": "XMLHttpRequest",
                                          "Content-Type": "application/json",
                                          # Cloudflare blocks Python's default User-Agent (error 1010)
                                          "User-Agent": "claude-board/1.0"})
    try:
        with urllib.request.urlopen(req) as r:
            data = r.read()
    except urllib.error.HTTPError as e:
        sys.exit(f"{method} {path}: HTTP {e.code} {e.read().decode()[:300]}")
    return json.loads(data) if data else None


_names = {}
def usernames(ids):
    missing = [i for i in ids if i not in _names]
    if missing:
        _names.update({u["id"]: u["username"] for u in call("POST", "/api/v4/users/ids", missing)})
    return [_names.get(i, i) for i in ids]


def fmt_props(board, props):
    out = {}
    for p in board["cardProperties"]:
        v = props.get(p["id"])
        if v in (None, "", []):
            continue
        if p["type"] in ("select", "multiSelect"):
            opts = {o["id"]: o["value"] for o in p["options"]}
            v = ", ".join(opts.get(x, x) for x in (v if isinstance(v, list) else [v]))
        elif p["type"] in ("person", "multiPerson"):
            v = ", ".join(usernames(v if isinstance(v, list) else [v]))
        elif p["type"] == "date":  # stored as '{"from": ms, "to": ms}'
            d = json.loads(v)
            v = " -> ".join(time.strftime("%Y-%m-%d", time.gmtime(d[k] / 1000)) for k in ("from", "to") if d.get(k))
        out[p["name"]] = v
    return out


def set_prop(card, pid, value):
    # updatedProperties replaces the whole map, so send every existing property along
    call("PATCH", f"/cards/{card['id']}", {"updatedProperties": {**card["properties"], pid: value}})


def status_option(board, name):
    prop = next(p for p in board["cardProperties"] if p["name"] == "Status")
    opt = next((o for o in prop["options"] if o["value"].lower() == name.lower()), None)
    if not opt:
        sys.exit(f"unknown status {name!r}; valid: {[o['value'] for o in prop['options']]}")
    return prop["id"], opt["id"]


def main(cmd="help", *a):
    if cmd == "me":
        print(call("GET", "/api/v4/users/me")["username"])
    elif cmd == "boards":
        for t in call("GET", "/api/v4/users/me/teams"):
            for b in call("GET", f"/teams/{t['id']}/boards"):
                print(b["id"], b["title"], sep="\t")
    elif cmd == "cards":
        board = call("GET", f"/boards/{a[0]}")
        cards = call("GET", f"/boards/{a[0]}/cards?page=0&per_page=500")
        for c in cards:
            p = fmt_props(board, c["properties"])
            if len(a) < 2 or p.get("Status", "").lower() == a[1].lower():
                print(c["id"], c["title"], json.dumps(p, ensure_ascii=False), sep="\t")
    elif cmd == "show":
        card = call("GET", f"/cards/{a[0]}")
        board = call("GET", f"/boards/{card['boardId']}")
        print("#", card["title"])
        props = fmt_props(board, card["properties"])
        props["Created by"] = usernames([card["createdBy"]])[0]
        props["Last updated"] = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(card["updateAt"] / 1000))
        print(json.dumps(props, ensure_ascii=False, indent=1))
        blocks = call("GET", f"/boards/{card['boardId']}/blocks?parent_id={card['id']}")
        order = {bid: i for i, bid in enumerate(card.get("contentOrder") or [])}
        for b in sorted(blocks, key=lambda b: (b["type"] == "comment", order.get(b["id"], 0), b["createAt"])):
            if b["type"] == "text":
                print("\n" + b["title"])
            elif b["type"] == "checkbox":
                print(f"- [{'x' if b['fields'].get('value') else ' '}] {b['title']}")
            elif b["type"] == "comment":
                print(f"\n> {usernames([b['createdBy']])[0]}: {b['title']}")
    elif cmd == "move":
        card = call("GET", f"/cards/{a[0]}")
        pid, oid = status_option(call("GET", f"/boards/{card['boardId']}"), a[1])
        set_prop(card, pid, oid)
        print("moved", card["title"], "->", a[1])
    elif cmd == "create":
        board = call("GET", f"/boards/{a[0]}")
        pid, oid = status_option(board, a[2] if len(a) > 2 else "Planned")
        card = call("POST", f"/boards/{a[0]}/cards", {"title": a[1], "properties": {pid: oid}, "contentOrder": []})
        print(card["id"], card["title"], sep="\t")
    elif cmd == "assign":
        card = call("GET", f"/cards/{a[0]}")
        board = call("GET", f"/boards/{card['boardId']}")
        pid = next(p["id"] for p in board["cardProperties"] if p["name"] == "Assignee")
        user = call("GET", "/api/v4/users/" + (f"username/{a[1]}" if len(a) > 1 else "me"))
        set_prop(card, pid, user["id"])
        print("assigned", card["title"], "->", user["username"])
    elif cmd in ("comment", "describe"):
        card = call("GET", f"/cards/{a[0]}")
        now = int(time.time() * 1000)
        block = call("POST", f"/boards/{card['boardId']}/blocks", [{
            "id": "", "boardId": card["boardId"], "parentId": card["id"], "type": "comment" if cmd == "comment" else "text",
            "title": a[1], "schema": 1, "fields": {}, "createAt": now, "updateAt": now}])[0]
        if cmd == "describe":  # description paragraphs only render once listed in the card's contentOrder
            call("PATCH", f"/cards/{card['id']}", {"contentOrder": (card.get("contentOrder") or []) + [block["id"]]})
        print(cmd, "added to", card["title"])
    else:
        print(__doc__)


if __name__ == "__main__":
    main(*sys.argv[1:])

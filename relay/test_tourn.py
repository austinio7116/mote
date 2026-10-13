#!/usr/bin/env python3
"""test_tourn.py -- the club's tournaments (mote_tourn.py) on a fake clock:
a whole night of eight to its champion, byes, a night called off, a no-show,
a late arrival, a drop-out waited for and one walked over, a disputed frame, a
relay restart in the middle, and the draw made again from the entry list.
Temp folders only.

    python3 relay/test_tourn.py
"""
import os, shutil, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mote_tourn as T  # noqa: E402

ok = bad = 0


def check(cond, what):
    global ok, bad
    print(("  [ok ] " if cond else "  [BAD] ") + what)
    ok += bool(cond); bad += not cond


class Clock:
    def __init__(self, t): self.t = t
    def __call__(self): return self.t


START = 2_000_000_000.0          # a Friday night, as far as anyone here knows


def night(n, store=None, size=8):
    """a tournament of n entrants, created a day ahead; returns (org, clock, tid, uids)"""
    store = store or tempfile.mkdtemp()
    clock = Clock(START - 86400)
    org = T.Organiser(store, now=clock)
    tid, why = org.create({"gid": "4355", "title": "UK 8-BALL KNOCKOUT", "kind": 0, "size": size,
                           "best_of": 3, "final_best_of": 5, "start": START, "ver": "7.5"})
    uids = [f"10{i}" for i in range(1, n + 1)]
    for i, u in enumerate(uids):
        org.enter(tid, u, f"PLAYER{i + 1}", "7.5")
    return org, clock, tid, uids, store


def step(org, clock, secs):
    clock.t += secs
    org.tick()


def play(org, t, m, winner_num, frames_to=None):
    """both to the table, then the winner takes the frames needed"""
    ua, ub = t.draw[m.a], t.draw[m.b]
    org.table_in(t.tid, m.mid, ua); org.table_in(t.tid, m.mid, ub)
    need = m.best_of // 2 + 1
    w = t.draw[winner_num]
    for _ in range(need):
        org.frame_agreed(t.tid, m.mid, w)


def edits():
    # ---- CHANGED WHILE IT TAKES ENTRIES (2026-10-13: Ultimate Pool, 16 places)
    org, clock, tid, uids, store = night(5)
    check(org.admin("EDIT", {"tid": tid, "rules": 2, "size": 16}) == "OK", "the admin changes the rules and the size")
    t = org.t[tid]
    check(t.rules == 2 and t.size == 16 and len(t.entrants) == 5, "the rules and size changed, the entrants kept")
    check(org.admin("EDIT", {"tid": tid, "size": 4}) == "TOOSMALL", "never fewer places than have entered")
    check(org.admin("EDIT", {"tid": tid, "size": 12}) == "BAD", "a size that is not a draw is refused")
    check(org.admin("EDIT", {"tid": tid}) == "BAD", "an edit that changes nothing is refused")
    check(org.admin("SHRUG", {"tid": tid}) == "UNKNOWN", "an admin verb it does not know")
    again = T.Organiser(store, now=clock)
    check(again.t[tid].rules == 2 and again.t[tid].size == 16 and len(again.t[tid].entrants) == 5,
          "the change is in the record: a restarted relay has it")
    for i in range(5, 12):
        org.enter(tid, f"20{i}", f"MORE{i}", "7.5.1")
    check(len(t.entrants) == 12, "more than eight sign up once it takes sixteen")
    # ADDED AND TAKEN OFF BY THE ADMIN (2026-10-13)
    check(org.admin("ADD", {"tid": tid, "uid": "777", "name": "MESSAGED"}) == "OK", "the admin adds a player by account")
    check(t.entered("777") and t.entrants[-1]["name"] == "MESSAGED", "...who is entered under their name")
    check(org.admin("ADD", {"tid": tid, "uid": "777", "name": "MESSAGED"}) == "ALREADY", "...once")
    check(org.admin("ADD", {"tid": tid, "uid": "not-a-number", "name": "X"}) == "BAD", "an account is a number")
    check(org.admin("ADD", {"tid": tid, "acct": "0123456789ab", "name": "X"}) == "NOACCOUNT", "an account hash nobody has is not found")
    n = len(t.entrants) - 1
    check(org.admin("REMOVE", {"tid": tid, "n": n, "name": "SOMEONE ELSE"}) == "CHANGED", "a remove that names the wrong player does nothing")
    check(org.admin("REMOVE", {"tid": tid, "n": n, "name": "MESSAGED"}) == "OK" and not t.entered("777"), "the admin takes a player off")
    clock.t = START - T.OPEN_BEFORE + 1; org.tick()
    check(org.admin("EDIT", {"tid": tid, "rules": 0}) == "NOTOPEN", "no changes once the room has opened")
    check(org.admin("ADD", {"tid": tid, "uid": "778", "name": "LATE"}) == "OK", "...but a player can still be added up to the draw")
    check(T.Organiser(store, now=clock).t[tid].entered("778"), "an added player is in the record")


def main():
    edits()
    # ---- A WHOLE NIGHT OF EIGHT ------------------------------------------
    org, clock, tid, uids, store = night(8)
    t = org.t[tid]
    # NO VERSION AT SIGN-UP (2026-10-13): any version gets as far as FULL
    check(org.enter(tid, "999", "LATE", "7.4.5") == "FULL", "an entry on another game version is not turned away for it")
    check(org.enter(tid, "998", "POINT", "7.5.1") == "FULL", "...nor a point release")
    check(T.same_game("1129661808", "1129661809") and not T.same_game("1129661808", "1297109760"),
          "a tournament is shown to every version of its game, and no other game")
    check(org.enter(tid, "999", "NINTH", "7.5") == "FULL", "a ninth entrant is turned away")
    check(org.enter(tid, uids[0], "AGAIN", "7.5") == "ALREADY", "an account enters once")
    clock.t = START - T.OPEN_BEFORE - 1; org.tick()
    check(t.status == "open", "the room is not open sixteen minutes before")
    step(org, clock, 2)
    check(t.status == "room" and ("OPEN" in [l for _, l in org.take()]), "the room opens fifteen minutes before, and says so")
    check(org.enter(tid, "998", "LATE", "7.5") == "CLOSED", "sign-up has closed once the room is open")
    for u in uids:
        org.room_in(tid, u)
    clock.t = START; org.tick()
    check(t.status == "running" and len(t.draw) == 8, "at the start the draw is made, all eight in it")
    out = [l for _, l in org.take()]
    calls = [l for l in out if l.startswith("CALL ")]
    check(len(calls) == 4 and sorted(int(c.split()[2]) for c in calls) == [1, 2, 3, 4],
          f"four first-round matches called to tables 1-4: {calls}")
    again = T.Organiser.seeded_draw(tid, [e["uid"] for e in t.entrants])
    check(again == t.draw, "the draw is made again from the entry list alone")
    r1 = [m for m in t.matches if m.rnd == 1]
    for m in r1:
        play(org, t, m, m.a)
    check(all(m.state == "done" and m.winner == m.a for m in r1), "round one played: the four winners through")
    org.tick()
    r2 = [m for m in t.matches if m.rnd == 2]
    check(all(m.state == "called" for m in r2) and {m.table for m in r2} <= {1, 2, 3, 4}, "the semi-finals are called")
    for m in r2:
        play(org, t, m, m.b)
    org.tick()
    fin = t.matches[-1]
    check(fin.state == "called" and fin.table == 1 and fin.best_of == 5, "the final is called to table 1, best of five")
    play(org, t, fin, fin.a)
    check(t.status == "done" and t.champion == fin.a, f"the champion is crowned: player {t.champion}")
    check(any(l.startswith("CHAMPION ") for _, l in org.take()), "...and the room is told")

    # ---- A RESTART TAKES IT UP WHERE IT WAS ------------------------------
    org2 = T.Organiser(store, now=clock)
    t2 = org2.t[tid]
    check(t2.status == "done" and t2.champion == t.champion and [m.view() for m in t2.matches] == [m.view() for m in t.matches],
          "the record replayed gives the same night, match for match")
    shutil.rmtree(store)

    # ---- A NIGHT OF SIXTEEN, ON FOUR TABLES ------------------------------
    # (Mark, 10-12: "we might find we have more than 8 players on our first
    # night"): eight first-round matches and four tables, so they are called
    # as tables come free, and the night still runs to its champion
    org, clock, tid, uids, store = night(16, size=16)
    t = org.t[tid]
    check(org.enter(tid, "997", "SEVENTEENTH", "7.5") == "FULL", "a seventeenth entrant is turned away")
    clock.t = START - T.OPEN_BEFORE; org.tick()
    for u in uids:
        org.room_in(tid, u)
    clock.t = START; org.tick()
    check(t.status == "running" and len(t.draw) == 16 and len(t.matches) == 15, "sixteen in the draw, fifteen matches in the bracket")
    out = [l for _, l in org.take()]
    calls = [l for l in out if l.startswith("CALL ")]
    check(len(calls) == 4 and sorted(int(c.split()[2]) for c in calls) == [1, 2, 3, 4], f"four of the eight first-round matches called, one a table: {calls}")
    played, rounds = 0, 0
    while t.status == "running" and played < 40:
        live = [m for m in t.matches if m.state == "called"]
        if not live:
            org.tick(); live = [m for m in t.matches if m.state == "called"]
            if not live:
                break
        for m in live:
            play(org, t, m, m.a); played += 1
        org.tick()
        check(len([m for m in t.matches if m.state == "called"]) <= 4, "never more matches called than there are tables")
    check(played == 15 and t.status == "done" and t.champion is not None, f"all fifteen matches played, a champion: player {t.champion}")
    again = T.Organiser.seeded_draw(tid, [e["uid"] for e in t.entrants])
    check(again == t.draw, "the draw of sixteen is made again from the entry list alone")
    shutil.rmtree(store)

    # ---- TWELVE ENTRANTS: A DRAW OF SIXTEEN, FOUR BYES ------------------
    org, clock, tid, uids, store = night(12, size=16)
    t = org.t[tid]
    clock.t = START - T.OPEN_BEFORE; org.tick()
    for u in uids:
        org.room_in(tid, u)
    clock.t = START; org.tick()
    r1 = [m for m in t.matches if m.rnd == 1]
    byes = [m for m in r1 if m.why == "bye"]
    check(len(r1) == 8 and len(byes) == 4 and all((m.a is None) != (m.b is None) for m in byes), "twelve in a draw of sixteen: four byes, one to a match")
    shutil.rmtree(store)

    # ---- SIX ENTRANTS: TWO BYES, NEVER TOGETHER ---------------------------
    org, clock, tid, uids, store = night(6)
    t = org.t[tid]
    clock.t = START - T.OPEN_BEFORE; org.tick()
    for u in uids:
        org.room_in(tid, u)
    clock.t = START; org.tick()
    r1 = [m for m in t.matches if m.rnd == 1]
    byes = [m for m in r1 if m.why == "bye"]
    check(len(byes) == 2 and all((m.a is None) != (m.b is None) for m in byes), "six in an eight draw: two byes, one to a match")
    check(len([m for m in r1 if m.state == "called"]) == 2, "only the two real first-round matches are called")
    shutil.rmtree(store)

    # ---- TOO FEW: CALLED OFF ----------------------------------------------
    org, clock, tid, uids, store = night(5)
    t = org.t[tid]
    clock.t = START - T.OPEN_BEFORE; org.tick()
    org.room_in(tid, uids[0]); org.room_in(tid, uids[1])
    clock.t = START; org.tick()
    check(t.status == "off" and "2" in t.why_off, f"two of five there: the night is called off ({t.why_off})")
    shutil.rmtree(store)

    # ---- A NO-SHOW, A LATE ARRIVAL ----------------------------------------
    org, clock, tid, uids, store = night(8)
    t = org.t[tid]
    clock.t = START - T.OPEN_BEFORE; org.tick()
    absent, late = uids[0], uids[1]
    for u in uids[2:]:
        org.room_in(tid, u)
    clock.t = START; org.tick()
    check(t.status == "running" and len(t.draw) == 8, "six of eight there: the night goes ahead, all eight drawn")
    na, nl = t.num_of(absent), t.num_of(late)
    ma = next(m for m in t.matches if na in (m.a, m.b))
    ml = next(m for m in t.matches if nl in (m.a, m.b))
    step(org, clock, 5 * 60)
    if ml is not ma:
        org.room_in(tid, late)
        step(org, clock, 1)
        check(ml.state == "called", "a player five minutes late is still called")
    step(org, clock, 5 * 60 + 1)
    if ma is not ml:
        check(ma.state == "done" and ma.why == "walkover" and ma.winner != na, "ten minutes gone: the absent player is walked over")
    else:
        check(ma.state == "done" and ma.why == "default", "both of a match absent: the lower number through by default")
    shutil.rmtree(store)

    # ---- DROPPING OUT: WAITED FOR, THEN WALKED OVER ------------------------
    org, clock, tid, uids, store = night(4)
    t = org.t[tid]
    clock.t = START - T.OPEN_BEFORE; org.tick()
    for u in uids:
        org.room_in(tid, u)
    clock.t = START; org.tick()
    m1, m2 = [m for m in t.matches if m.rnd == 1]
    for m in (m1, m2):
        org.table_in(tid, m.mid, t.draw[m.a]); org.table_in(tid, m.mid, t.draw[m.b])
    check(m1.state == "playing" and m2.state == "playing", "both at their tables: the matches are playing")
    org.frame_agreed(tid, m1.mid, t.draw[m1.a])
    check(m1.score == [1, 0], "an agreed frame is on the score")
    org.room_out(tid, t.draw[m1.b])
    check(m1.state == "paused", "a player drops out: the match is paused, not given to the computer")
    step(org, clock, 3 * 60)
    org.room_in(tid, t.draw[m1.b]); org.table_in(tid, m1.mid, t.draw[m1.b])
    check(m1.state == "playing", "back within five minutes: playing again")
    org.room_out(tid, t.draw[m2.a])
    step(org, clock, 4 * 60)
    check(m2.state == "paused", "four minutes gone: still waited for")
    step(org, clock, 61)
    check(m2.state == "done" and m2.why == "walkover" and m2.winner == m2.b, "five minutes gone: the one who stayed goes through")
    org.frame_disputed(tid, m1.mid, 2)
    check(any(l == f"REPLAY {m1.mid} 2" for _, l in org.take()), "a disputed frame is sent back to be replayed")
    # ...and a restart in the middle of the match
    org2 = T.Organiser(store, now=clock)
    t2 = org2.t[tid]
    mm = next(m for m in t2.matches if m.mid == m1.mid)
    check(mm.state == "paused" and mm.score == [1, 0], "a restart mid-match: the score kept, the match waiting for them")
    for u in uids:
        org2.room_in(tid, u)
    org2.table_in(tid, mm.mid, t2.draw[mm.a]); org2.table_in(tid, mm.mid, t2.draw[mm.b])
    check(mm.state == "playing", "...and playing again when both are back")
    shutil.rmtree(store)

    # ---- WITHDRAWING, AND THE LISTING ---------------------------------------
    org, clock, tid, uids, store = night(3)
    check(org.withdraw(tid, uids[2]) == "OK" and len(org.t[tid].entrants) == 2, "an entrant withdraws before the start")
    lst = org.listing("4355")
    check(len(lst) == 1 and lst[0]["entrants"] == ["PLAYER1", "PLAYER2"] and "101" not in str(lst), "the listing names entrants, never accounts")
    check(org.create({"kind": 0, "start": clock.t - 10})[1] == "PAST", "a tournament cannot be made in the past")
    shutil.rmtree(store)

    print(f"{ok} ok, {bad} bad")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

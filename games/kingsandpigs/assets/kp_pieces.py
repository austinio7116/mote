#!/usr/bin/env python3
"""Kings & Pigs room-piece library (carve-authored, all pass proto_check)."""
PIECES = {
    "start": {"kind":"start","rows":[
        "###########",
        "#.........#",
        "#....#=#..#",
        "#....#.#.=>",
        "#.e.#...dE>",
        "#####vv####"]},
    "exit": {"kind":"exit","rows":[
        "####^^####",
        "#....=...#",
        "#..d.....#",
        "<...##.d.#",
        "<.E.##.x.#",
        "##########"]},
    "pillar_hall": {"kind":"side","rows":[
        "##^^#######",
        "#....d.W..#",
        "#.=.F##...#",
        "#....##.d.#",
        "<..=.##--.>",
        "<..B##..E.>",
        "######vv###"]},
    "staircase": {"kind":"side","rows":[
        "######^^##",
        "#....F...#",
        "#...d.=..#",
        "#.d.#....#",
        "<..##..=.>",
        "<.###..hE>",
        "######vv##"]},
    "twin_pockets": {"kind":"side","rows":[
        "########^^##",
        "#....##....#",
        "#.d-.##.=..#",
        "<.......Wd.>",
        "<.#d.....E.>",
        "##vv########"]},
    "arch_hall": {"kind":"side","rows":[
        "#####^^#####",
        "#.W......F.#",
        "#....=.....#",
        "#....dd...E#",
        "<..#=..=#..>",
        "<.B#....#..>",
        "#####vv#####"]},
    "alcove_vault": {"kind":"side","rows":[
        "##########",
        "#.dd.....#",
        "#.===.hD.#",
        "<....---.#",
        "<.B..##.d#",
        "##########"]},
    "shaft_ledges": {"kind":"side","rows":[
        "###^^####",
        "#....W..#",
        "#d.=....#",
        "###..--E#",
        "#...=.==#",
        "#.......>",
        "#.B..#d.>",
        "#####vv##"]},
}

if __name__=="__main__":
    import sys,os; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
    import proto_check as C
    bad=0
    for n,d in PIECES.items():
        e=C.check_piece(d["rows"]); print(n,"OK" if not e else e[:2]); bad+=bool(e)
    print("ALL OK" if not bad else f"{bad} bad")

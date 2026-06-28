/*
 * ThumbyElite — event pool + lore fragments (pure const data; the
 * interpreter lives in events.c). Conventions:
 *   - OP_BRANCH a b = a% chance to jump to op INDEX b of the same list.
 *   - Costs live on the Choice (deducted up-front; a wager's win op
 *     must therefore return stake + winnings).
 *   - $-tokens expand in body AND result texts ($N npc, $S system,
 *     $T station, $F faction, $G trade good).
 * THE POLICY (main thread): act 1 the Underwriter (flags 8/10/11, lore
 * 0-2), act 2 the Beneficiary (flags 14/15/16, lore 8-10), act 3 the
 * Terms (flags 17/18/19, lore 11-13). Side lore: 3 prospector, 4 your
 * file, 5 the cover, 6 the pod, 7 recalled, 14 receipt, 15 arrears,
 * 16 continued, 17 the wall.
 * Story flags: 1 stowaway rode, 2 preacher's token, 3 purged file,
 * 12 opened pod, 13 stowaway repaid, 20 kept the token, 21 read your
 * file (gates the memorial).
 */
#include "events.h"
#include <stddef.h>

/* --- 1 DISTRESS HAIL ---------------------------------------------------- */
static const Op e1_give[]   = { {OP_FUEL,-10,0}, {OP_REP,-1,4},
                                {OP_LATER,6,0},     /* they repay you */
                                {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e1_demand[] = { {OP_BRANCH,60,3}, {OP_RESULT,1,0}, {OP_END,0,0},
                                {OP_FUEL,-10,0}, {OP_CR,8,0},
                                {OP_RESULT,2,0}, {OP_END,0,0} };
static const Op e1_turn[]   = { {OP_REP,-1,-2}, {OP_RESULT,3,0}, {OP_END,0,0} };
static const char *const e1_tx[] = {
    "THEY LIMP INTO $T BURNING YOUR GIFT. WORD OF IT TRAVELS.",
    "'NOTHING LEFT TO PAY WITH.' THE CHANNEL GOES QUIET.",
    "200 CR FOR A TANK OF HYDROGEN. FAIR IS FAIR.",
    "YOU CUT THE CHANNEL. THE BAY LIGHTS FEEL COLDER.",
};
static const Choice e1_ch[] = {
    { "GIVE THEM FUEL",  GATE_FUEL_SPARE, 0, e1_give },
    { "DEMAND PAYMENT",  GATE_FUEL_SPARE, 0, e1_demand },
    { "TURN AWAY",       0,               0, e1_turn },
};

/* --- 2 CUSTOMS SWEEP ---------------------------------------------------- */
static const Op e2_submit[] = { {OP_CONTRA,0,0}, {OP_CR,-6,0},
                                {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e2_bribe[]  = { {OP_BRANCH,70,5}, {OP_CONTRA,0,0},
                                {OP_LEGAL,1,0}, {OP_RESULT,2,0}, {OP_END,0,0},
                                {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e2_dump[]   = { {OP_CONTRA,0,0}, {OP_RESULT,3,0}, {OP_END,0,0} };
static const char *const e2_tx[] = {
    "THE HOLD IS STRIPPED AND A 150 CR FINE FILED. THE INSPECTOR ALMOST LOOKS SORRY.",
    "THE CREDITS VANISH INTO A GLOVE. 'PAPERWORK ERROR. HAPPENS.'",
    "WRONG OFFICER. THE HOLD IS SEIZED AND YOUR NAME GOES ON A LIST.",
    "CANISTERS TUMBLE OFF THE GANTRY INTO THE RECYCLER. PROOF GONE - PROFIT TOO.",
};
static const Choice e2_ch[] = {
    { "SUBMIT TO SEARCH",   0, 0,   e2_submit },
    { "BRIBE THE INSPECTOR",0, 300, e2_bribe },
    { "DUMP IT AND DENY",   0, 0,   e2_dump },
};

/* --- 3 THE PROSPECTOR'S CLAIM (oneshot, lore) --------------------------- */
static const Op e3_buy[]  = { {OP_BRANCH,70,3}, {OP_RESULT,1,0}, {OP_END,0,0},
                              {OP_LORE,3,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e3_wave[] = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e3_tx[] = {
    "COORDINATES POINTING CLEAN OUT OF THE GALAXY. STAMPED ACROSS THEM: CLAIM RECORDED - THE INDEMNITY.",
    "JUNK TELEMETRY. THE MINER IS GONE BY THE TIME YOU LOOK UP.",
    "HE SHUFFLES OFF TO THE NEXT PILOT. SOMETHING IN HIS EYES STAYS WITH YOU.",
};
static const Choice e3_ch[] = {
    { "BUY THE DATA", 0, 150, e3_buy },
    { "WAVE HIM OFF", 0, 0,   e3_wave },
};

/* --- 4 STOWAWAY --------------------------------------------------------- */
static const Op e4_hand[] = { {OP_REP,-1,3}, {OP_CR,2,0},
                              {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e4_ride[] = { {OP_FLAG,1,0}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e4_fare[] = { {OP_CR,4,0}, {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e4_tx[] = {
    "STATION SECURITY LOGS THE FARE-DODGER. A SMALL BOUNTY LANDS IN YOUR ACCOUNT.",
    "THEY SCRUB YOUR DECK PLATES TO A SHINE AND VANISH AT THE NEXT AIRLOCK. 'THE INDEMNITY KEEPS YOU,' THEY SAY. ODD BLESSING.",
    "A HUNDRED IN CRUMPLED CHITS. EVERYONE PAYS THEIR PREMIUM EVENTUALLY.",
};
static const Choice e4_ch[] = {
    { "HAND THEM OVER",    0, 0, e4_hand },
    { "LET THEM WORK PASSAGE", 0, 0, e4_ride },
    { "CHARGE THEM FARE",  0, 0, e4_fare },
};

/* --- 5 PIRATE TOLL ------------------------------------------------------ */
static const Op e5_pay[]    = { {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e5_refuse[] = { {OP_BRANCH,55,4}, {OP_AMBUSH,2,1},
                                {OP_RESULT,2,0}, {OP_END,0,0},
                                {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e5_threat[] = { {OP_RESULT,3,0}, {OP_END,0,0} };
static const char *const e5_tx[] = {
    "THE FACE GRINS. 'PLEASURE INSURING YOU.' THE CHANNEL DIES.",
    "SILENCE. MAYBE A BLUFF. MAYBE THEY'RE PATIENT.",
    "'WRONG ANSWER.' SCOPES PICK UP TWO CONTACTS TAKING POSITION OUTSIDE THE BAY.",
    "YOU QUOTE YOUR OWN BOUNTY AT THEM. THE FACE PALES AND CUTS THE LINK.",
};
static const Choice e5_ch[] = {
    { "PAY THE TOLL",      0,           200, e5_pay },
    { "REFUSE",            0,           0,   e5_refuse },
    { "THREATEN THEM BACK",GATE_WANTED, 0,   e5_threat },
};

/* --- 6 OVERSTOCK FIRE SALE ---------------------------------------------- */
static const Op e6_buy[]  = { {OP_CARGO,-1,4}, {OP_BRANCH,15,4},
                              {OP_RESULT,0,0}, {OP_END,0,0},
                              {OP_LEGAL,1,0}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e6_pass[] = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e6_tx[] = {
    "FOUR CRATES, CLEAN PAPERS. A GENUINE BARGAIN FOR ONCE.",
    "THE CRATES SCAN AS STOLEN FREIGHT TWO SYSTEMS OVER. NOW IT'S YOUR PROBLEM.",
    "THE PALLET IS GONE WITHIN THE HOUR. SO IS THE TRADER.",
};
static const Choice e6_ch[] = {
    { "BUY THE LOT",        0, 200, e6_buy },
    { "TOO GOOD TO BE TRUE",0, 0,   e6_pass },
};

/* --- 7 PREACHER OF THE COVER (oneshot, lore) ----------------------------- */
static const Op e7_listen[] = { {OP_LORE,5,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e7_donate[] = { {OP_LORE,5,0}, {OP_FLAG,2,0},
                                {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e7_move[]   = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e7_tx[] = {
    "'NOBODY SIGNED. NOBODY READS THE TERMS. BUT MISS A PAYMENT AND SEE HOW FAST IT NOTICES YOU.' THE CROWD WON'T MEET YOUR EYES.",
    "THE PREACHER PRESSES A COLD TOKEN INTO YOUR PALM. 'A RECEIPT. IT REMEMBERS WHO KEEPS CURRENT.'",
    "THE SERMON FOLLOWS YOU DOWN THE GANTRY UNTIL THE AIRLOCK CUTS IT OFF.",
};
static const Choice e7_ch[] = {
    { "LISTEN",     0, 0,  e7_listen },
    { "DONATE",     0, 25, e7_donate },
    { "MOVE ALONG", 0, 0,  e7_move },
};

/* --- 8 CLINIC SHORTFALL -------------------------------------------------- */
static const Op e8_meds[] = { {OP_CARGO,5,-1}, {OP_REP,-1,5}, {OP_DMG,-20,0},
                              {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e8_fund[] = { {OP_REP,-1,3}, {OP_DMG,-25,0},
                              {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e8_walk[] = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e8_tx[] = {
    "YOUR CRATE OF MEDICINE DISAPPEARS INTO THE WARD. THE MEDIC GRIPS YOUR ARM. PEOPLE HERE WILL REMEMBER YOUR HULL.",
    "CREDITS BUY WHAT THE NEXT FREIGHTER CARRIES. THE TRIAGE QUEUE SHUFFLES FORWARD.",
    "YOU STEP AROUND THE STRETCHERS. THE SMELL OF ANTISEPTIC FOLLOWS YOU TO THE BAY.",
};
static const Choice e8_ch[] = {
    { "DONATE MEDICINE", GATE_HAS_MEDS, 0,   e8_meds },
    { "FUND THE WARD",   0,             100, e8_fund },
    { "NOT YOUR PROBLEM",0,             0,   e8_walk },
};

/* --- 9 THE REGISTRY FILE (oneshot, lore, no portrait) -------------------- */
static const Op e9_read[]   = { {OP_LORE,4,0}, {OP_FLAG,21,0},
                                {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e9_delete[] = { {OP_FLAG,3,0}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e9_tx[] = {
    "A SHIP OF YOUR EXACT CLASS, LOST WITH ALL HANDS - FILED FORTY YEARS AGO. THE REGISTERED OWNER IS YOU. POLICY STATUS: CURRENT.",
    "YOU PURGE THE RECORD. AT THE EDGE OF THE SCREEN, FOR HALF A SECOND: RESUBMITTED.",
};
static const Choice e9_ch[] = {
    { "READ THE FILE", 0, 0, e9_read },
    { "DELETE IT",     0, 0, e9_delete },
};

/* --- 10 DOCKSIDE WAGER --------------------------------------------------- */
static const Op e10_bet[]  = { {OP_BRANCH,50,3}, {OP_RESULT,1,0}, {OP_END,0,0},
                               {OP_CR,12,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e10_pass[] = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e10_tx[] = {
    "YOU SHAVE THE BEACON SO CLOSE THE PROXIMITY ALARM SINGS. THE RACER PAYS UP, LAUGHING.",
    "THEIR BURN IS FILTHY AND PERFECT. YOU PAY UP. THE DOCK CREW SAW NOTHING, THEY PROMISE.",
    "'SMART. NOBODY BEATS ME.' THE RACER SAUNTERS OFF TO FIND BRAVER MONEY.",
};
static const Choice e10_ch[] = {
    { "TAKE THE BET", 0, 150, e10_bet },
    { "DECLINE",      0, 0,   e10_pass },
};

/* ======================= THE UNDERWRITER (arc, bar/dock) ====================
 * Recurring grey-coat NPC (fixed_npc 1). Flags: 8 met them, 10 retained,
 * 11 arc complete. Reveals lore 0 -> 1 -> 2 in order. */

/* --- 11 A STRANGER IN GREY (step 1, bar) -------------------------------- */
static const Op e11_hear[]  = { {OP_FLAG,8,0}, {OP_LORE,0,0},
                                {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e11_brush[] = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e11_tx[] = {
    "'EVERY FEE YOU PAY, A FRACTION GOES UP. CALL IT COVER. YOU'LL WANT IT, WHERE YOU FLY.' THEY LEAVE NO GLASS, NO NAME.",
    "THEY NOD AS IF YOU'D SIGNED SOMETHING ANYWAY, AND GO.",
};
static const Choice e11_ch[] = {
    { "HEAR THEM OUT",  0, 0, e11_hear },
    { "BRUSH THEM OFF", 0, 0, e11_brush },
};

/* --- 12 THE RETAINER (step 2, bar) --------------------------------------- */
static const Op e12_take[]  = { {OP_CR,12,0}, {OP_FLAG,10,0}, {OP_LORE,1,0},
                                {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e12_press[] = { {OP_FLAG,10,0}, {OP_LORE,1,0},
                                {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e12_walk[]  = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e12_tx[] = {
    "CREDITS ARRIVE FROM AN ACCOUNT THAT DOESN'T EXIST. THE WRECK THEY NAMED ISN'T ON ANY SCHEDULE.",
    "'WE DON'T CAUSE THE LOSSES. WE COVER THEM.' BY THE TIME YOU BLINK, THE STOOL IS EMPTY.",
    "YOU LEAVE YOUR DRINK. SOME JOBS SMELL LIKE BAD WEATHER.",
};
static const Choice e12_ch[] = {
    { "TAKE THE RETAINER", 0, 0, e12_take },
    { "PRESS FOR ANSWERS", 0, 0, e12_press },
    { "WALK AWAY",         0, 0, e12_walk },
};

/* --- 13 CLAIM ADJUSTED (step 3, dock, oneshot) ---------------------------- */
static const Op e13_demand[] = { {OP_LORE,2,0}, {OP_FLAG,11,0},
                                 {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e13_report[] = { {OP_REP,-1,3}, {OP_CR,8,0}, {OP_FLAG,11,0},
                                 {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e13_quiet[]  = { {OP_CR,20,0}, {OP_FLAG,11,0},
                                 {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e13_tx[] = {
    "'CLAIM SETTLED IN FULL,' IS ALL THE GREY PILOT SAYS. 'THE COLONY CASE? BEFORE MY TIME.' YOU DIDN'T MENTION ANY COLONY.",
    "SECURITY BOARDS THE GREY SHIP AND FINDS NOBODY ABOARD. NO LOGS. NO SEATS.",
    "AN UNSIGNED TRANSFER LANDS IN YOUR ACCOUNT: 'FOR DISCRETION.' THE GREY SHIP IS GONE BY MORNING.",
};
static const Choice e13_ch[] = {
    { "DEMAND ANSWERS",  0, 0, e13_demand },
    { "REPORT THE SHIP", 0, 0, e13_report },
    { "SAY NOTHING",     0, 0, e13_quiet },
};

/* ======================= bar pool ======================================== */

/* --- 14 CARD GAME --------------------------------------------------------- */
static const Op e14_play[] = { {OP_BRANCH,55,3}, {OP_RESULT,1,0}, {OP_END,0,0},
                               {OP_CR,8,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e14_pass[] = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e14_tx[] = {
    "YOUR LAST CARD LANDS LIKE A DOCKING CLAMP. THE TABLE PAYS, GRUMBLING.",
    "THE DEALER'S SMILE NEVER MOVES. NEITHER DO YOUR CREDITS - AWAY FROM YOU.",
    "YOU KEEP YOUR CREDITS IN YOUR POCKET AND YOUR BACK TO THE WALL.",
};
static const Choice e14_ch[] = {
    { "SIT IN",   0, 100, e14_play },
    { "JUST WATCH", 0, 0, e14_pass },
};

/* --- 15 THE NAVIGATOR'S TRICK --------------------------------------------- */
static const Op e15_buy[]  = { {OP_FUEL,50,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e15_pass[] = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e15_tx[] = {
    "SHE SKETCHES A SCOOP APPROACH ON A NAPKIN THAT SAVES YOU REAL HYDROGEN. CHEAPEST FUEL YOU EVER BOUGHT.",
    "SHE SHRUGS AND SELLS THE NAPKIN TO THE NEXT BOOTH.",
};
static const Choice e15_ch[] = {
    { "BUY HER A ROUND", 0, 40, e15_buy },
    { "NOT TONIGHT",     0, 0,  e15_pass },
};

/* --- 16 OLD WAR STORY ------------------------------------------------------ */
static const Op e16_listen[] = { {OP_REP,-1,2}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e16_round[]  = { {OP_REP,-1,4}, {OP_CARGO,3,1},
                                 {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e16_leave[]  = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e16_tx[] = {
    "BY THE THIRD TELLING THE ODDS WERE WORSE AND THE ESCAPE NARROWER. THE LOCALS APPROVE OF YOUR PATIENCE.",
    "THE WHOLE CORNER DRINKS TO $F AND, SOMEHOW, TO YOU. A BOTTLE FOR THE ROAD FINDS ITS WAY INTO YOUR HOLD.",
    "THE STORY FOLLOWS YOU OUT THE DOOR. IT WAS LOUDER THAN THE MUSIC.",
};
static const Choice e16_ch[] = {
    { "LISTEN",        0, 0,  e16_listen },
    { "BUY THE ROUND", 0, 25, e16_round },
    { "SLIP AWAY",     0, 0,  e16_leave },
};

/* --- 17 THE FIXER (rough space, wanted pilots) ------------------------------ */
static const Op e17_pay[]  = { {OP_BRANCH,80,4}, {OP_RESULT,1,0}, {OP_END,0,0},
                               {OP_END,0,0},
                               {OP_LEGAL,-2,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e17_pass[] = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e17_tx[] = {
    "BY MORNING YOUR WARRANTS HAVE BEEN 'MISFILED'. THE FIXER DOESN'T SAY WHERE. YOU DON'T ASK.",
    "THE FIXER AND YOUR CREDITS LEAVE BY DIFFERENT DOORS. YOUR RECORD STAYS EXACTLY WHERE IT WAS.",
    "'SUIT YOURSELF. THE LAW'S MEMORY IS LONG, AND I'M CHEAP AT TWICE THE PRICE.'",
};
static const Choice e17_ch[] = {
    { "PAY THE FIXER", 0, 400, e17_pay },
    { "WALK ON",       0, 0,   e17_pass },
};

/* ======================= in-space: boarding a derelict ==================== */

/* --- 18 COLD HULL (repeatable) -------------------------------------------- */
static const Op e18_strip[] = { {OP_BRANCH,60,5}, {OP_BRANCH,38,12},
                                {OP_RESULT,1,0}, {OP_END,0,0}, {OP_END,0,0},
                                /* loot: crates, 35% with hardware too */
                                {OP_CARGO,-1,2}, {OP_BRANCH,35,9},
                                {OP_RESULT,0,0}, {OP_END,0,0},
                                {OP_ITEM,0,0}, {OP_RESULT,0,0}, {OP_END,0,0},
                                /* the lure */
                                {OP_AMBUSH,2,1}, {OP_RESULT,2,0}, {OP_END,0,0} };
static const Op e18_rec[]   = { {OP_BRANCH,55,4}, {OP_CR,4,0}, {OP_RESULT,4,0},
                                {OP_END,0,0},
                                {OP_LORE,1,0}, {OP_RESULT,3,0}, {OP_END,0,0} };
static const Op e18_leave[] = { {OP_RESULT,5,0}, {OP_END,0,0} };
static const char *const e18_tx[] = {
    "TWO CRATES COME FREE OF THE WRECKAGE, SEALS INTACT. THE REST IS SLAG AND SILENCE.",
    "THE HOLD WAS STRIPPED LONG BEFORE YOU GOT HERE. EVEN THE DECK PLATING IS GONE.",
    "THE 'WRECK' LIGHTS UP - A LURE. TWO CONTACTS BURN IN FROM THE SHADOW OF THE DEBRIS.",
    "THE RECORDER'S LAST ENTRY: A PLAIN GREY SHIP HOLDING STATION OFF THE BOW. LOGGED SIX HOURS BEFORE THE HULL BREACH.",
    "THE LOGS ARE MUNDANE - BUT COMPLETE, AND SALVAGE REGISTRIES PAY FOR CLOSURE. 100 CR.",
    "YOU LEAVE THE DEAD THEIR QUIET. THE HULK TUMBLES ON BEHIND YOU.",
};
static const Choice e18_ch[] = {
    { "STRIP THE HOLD",    GATE_CARGO_SPACE, 0, e18_strip },
    { "PULL THE RECORDER", 0,                0, e18_rec },
    { "LEAVE IT BE",       0,                0, e18_leave },
};

/* --- 19 THE LAST POD (oneshot) --------------------------------------------- */
static const Op e19_open[] = { {OP_LORE,6,0}, {OP_FLAG,12,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e19_scan[] = { {OP_LORE,6,0}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e19_go[]   = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e19_tx[] = {
    "THE POD IS EMPTY. WARM. THE MANIFEST LISTS ONE OCCUPANT: YOUR NAME, YOUR PRINTS, YOUR BLOOD TYPE. VACATED, IT SAYS. RECENTLY.",
    "THE SCAN COMPLETES AND THE POD SEALS ITSELF. THE MANIFEST UPLOADS SOMEWHERE YOU CAN'T TRACE - MARKED 'CLAIM CONTINUED'.",
    "YOU BURN AWAY AND DON'T LOOK BACK. THE CYCLING LIGHT OF THE POD BLINKS IN YOUR MIRRORS FOR LONGER THAN PHYSICS ALLOWS.",
};
static const Choice e19_ch[] = {
    { "OPEN IT",       0, 0, e19_open },
    { "SCAN AND SEAL", 0, 0, e19_scan },
    { "LEAVE - NOW",   0, 0, e19_go },
};

/* --- 20 STILL WARM (dangerous space) ---------------------------------------- */
static const Op e20_grab[] = { {OP_CARGO,-1,2}, {OP_BRANCH,40,4},
                               {OP_RESULT,0,0}, {OP_END,0,0},
                               {OP_AMBUSH,2,2}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e20_burn[] = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e20_tx[] = {
    "TWO CRATES, QUICK AND QUIET. THE HULL PINGS AS IT COOLS - THIS KILL IS MINUTES OLD.",
    "A CRATE ABOARD AND THE KILLERS COME BACK FOR THEIR LEAVINGS. THEY DON'T LOOK PLEASED TO SHARE.",
    "FRESH WRECK, NO BODIES, GUNS STILL COOLING SOMEWHERE CLOSE. YOU WERE NEVER HERE.",
};
static const Choice e20_ch[] = {
    { "GRAB AND GO", GATE_CARGO_SPACE, 0, e20_grab },
    { "BURN AWAY",   0,                0, e20_burn },
};

/* --- 21 GREY PAINT (post-arc, oneshot) --------------------------------------- */
static const Op e21_board[] = { {OP_LORE,7,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e21_leave[] = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e21_tx[] = {
    "GREY PLATES. NO REGISTRY. SYSTEMS WIPED CLEANER THAN ANY SALVAGER WORKS. THE UNDERWRITERS DECOMMISSION THEIR OWN - AND LEAVE NOTHING TO CLAIM.",
    "YOU KNOW THAT PAINT. YOU KEEP YOUR DISTANCE, AND THE DEAD GREY HULL KEEPS ITS SECRETS.",
};
static const Choice e21_ch[] = {
    { "BOARD HER",     0, 0, e21_board },
    { "KEEP CLEAR",    0, 0, e21_leave },
};

/* ======================= arrival hails ====================================== */

/* --- 22 PATROL CHALLENGE (lawful, carrying contraband) ----------------------- */
static const Op e22_comply[] = { {OP_CONTRA,0,0}, {OP_CR,-6,0},
                                 {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e22_run[]    = { {OP_BRANCH,50,3}, {OP_LEGAL,1,0},
                                 {OP_RESULT,2,0}, {OP_END,0,0},
                                 {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e22_tx[] = {
    "THE PATROL TAKES THE CARGO AND 150 CR IN FINES. 'WISE CHOICE. SAFE LANES.'",
    "YOU CUT THRUST, DRIFT DARK, AND THE PATROL'S SWEEP SLIDES PAST. THIS TIME.",
    "THE SWEEP PINS YOUR TRANSPONDER MID-BURN. YOUR NAME GOES ON THE WIRE.",
};
static const Choice e22_ch[] = {
    { "HEAVE TO",   0, 0, e22_comply },
    { "RUN FOR IT", 0, 0, e22_run },
};

/* --- 23 ROUTINE SWEEP (lawful, clean) ----------------------------------------- */
static const Op e23_tx_ops[] = { {OP_REP,-1,1}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e23_ig[]     = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e23_tx[] = {
    "'MANIFEST CLEAN. APPRECIATED, PILOT.' THE PATROL LOGS YOU AS A FRIENDLY AND PEELS AWAY.",
    "YOU LET THE REQUEST EXPIRE. THE PATROL SHADOWS YOU A WHILE LONGER THAN FEELS POLITE.",
};
static const Choice e23_ch[] = {
    { "TRANSMIT MANIFEST", 0, 0, e23_tx_ops },
    { "IGNORE THEM",       0, 0, e23_ig },
};

/* --- 24 WAYLAID (dangerous space) ----------------------------------------------*/
static const Op e24_pay[]    = { {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e24_refuse[] = { {OP_BRANCH,50,4}, {OP_AMBUSH,2,1},
                                 {OP_RESULT,2,0}, {OP_END,0,0},
                                 {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e24_tx[] = {
    "THE ESCORT FEE CLEARS AND THE CONTACTS FADE OFF YOUR SCOPE. CHEAPER THAN A HULL.",
    "A LONG SILENCE, THEN THE CONTACTS BREAK OFF. SOMEBODY ELSE'S SCOPE LOOKED SOFTER.",
    "'YOUR FUNERAL.' THE CONTACTS TURN IN, BURNING HARD.",
};
static const Choice e24_ch[] = {
    { "PAY THE ESCORT FEE", 0, 150, e24_pay },
    { "REFUSE",             0, 0,   e24_refuse },
};

/* --- 25 DRIFTING TRADER ----------------------------------------------------------*/
static const Op e25_buy[]  = { {OP_CARGO,-1,2}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e25_pass[] = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e25_tx[] = {
    "TWO CRATES OF $G CROSS ON A TETHER LINE, NO PAPERWORK, NO QUESTIONS. EVERYONE WAVES.",
    "THE TRADER SHRUGS ACROSS THE VOID AND DRIFTS BACK INTO THE DARK LANE TRAFFIC NEVER USES.",
};
static const Choice e25_ch[] = {
    { "TAKE THE DEAL", GATE_CARGO_SPACE, 120, e25_buy },
    { "FLY ON",        0,                0,   e25_pass },
};

/* ======================= continuity ============================================ */

/* --- 26 A FAMILIAR FACE (stowaway repaid, bar, oneshot) -------------------------- */
static const Op e26_take[] = { {OP_CR,6,0}, {OP_FLAG,13,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e26_wave[] = { {OP_REP,-1,3}, {OP_FLAG,13,0},
                               {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e26_tx[] = {
    "'FIRST WAGES.' THE KID FROM YOUR CARGO RACKS - TALLER NOW, CREW PATCHES ON THE SLEEVE - COUNTS OUT 150 CR AND WON'T TAKE NO.",
    "YOU WAVE THE CREDITS OFF. THE KID GRINS AND BUYS THE WHOLE BAR A ROUND IN YOUR NAME INSTEAD.",
};
static const Choice e26_ch[] = {
    { "TAKE THE WAGES", 0, 0, e26_take },
    { "KEEP THEM",      0, 0, e26_wave },
};

/* --- 27 THE RECRUITER (faction war, near the front) ----------------------- */
static const Op e27_sign[] = { {OP_MISSION,0,0}, {OP_CR,4,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e27_pass[] = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e27_tx[] = {
    "A SIGNING BONUS HITS YOUR ACCOUNT AND A WAR CONTRACT HITS YOUR LOG. 'HOLD THE LINE, PILOT. WE'LL KNOW IF YOU DON'T.'",
    "THE RECRUITER'S EYES ARE ALREADY ON THE NEXT PILOT DOWN THE GANTRY. THE WAR DOESN'T WAIT.",
};
static const Choice e27_ch[] = {
    { "SIGN ON",      0, 0, e27_sign },
    { "NOT YOUR WAR", 0, 0, e27_pass },
};


/* ================= THE POLICY, ACT 2: THE BENEFICIARY ===================
 * Vessa the archivist (fixed_npc 2). Gated on Act 1 (flag 11).
 * Flags: 14 met her, 15 saw the ledger, 16 act complete.
 * The question Act 1 left: WHAT is the Indemnity? Act 2 makes it worse:
 * it isn't about the galaxy. It's about you. */

/* --- 28 THE ARCHIVIST (bar, act 2 step 1) -------------------------------- */
static const Op e28_listen[] = { {OP_LORE,8,0}, {OP_FLAG,14,0},
                                 {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e28_leave[]  = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e28_tx[] = {
    "'CLAIMS THAT NEVER LAPSE. PREMIUMS FROM DEAD ACCOUNTS, ON TIME, FOREVER. I'VE FOUND ELEVEN.' SHE TAPS HER SLATE. 'YOURS MAKES TWELVE.'",
    "SHE DOESN'T WATCH YOU GO. SHE'S ALREADY BACK IN THE LEDGERS, AND SOMETHING ABOUT THAT IS WORSE.",
};
static const Choice e28_ch[] = {
    { "HEAR HER OUT", 0, 0, e28_listen },
    { "WALK AWAY",    0, 0, e28_leave },
};

/* --- 29 THE LEDGER (bar, act 2 step 2) ----------------------------------- */
static const Op e29_look[] = { {OP_LORE,9,0}, {OP_FLAG,15,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e29_fund[] = { {OP_LORE,9,0}, {OP_FLAG,15,0}, {OP_REP,-1,2},
                               {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e29_no[]   = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e29_tx[] = {
    "SHE TURNS THE SLATE AROUND. EVERY FEE YOU'VE EVER PAID, A FRACTION ROUTED UP - LARGER THAN ANYONE'S. 'YOU'RE PAYING A DEATH CLAIM,' SHE SAYS. 'THE NAME ON THE CLAIM IS YOURS.'",
    "YOUR CREDITS BUY HER ARCHIVE TIME SHE COULDN'T AFFORD. THE TRACE COMES BACK CLEAN, TWICE. IT'S YOUR CLAIM. IT'S ALWAYS BEEN YOUR CLAIM.",
    "'SUIT YOURSELF.' SHE CLOSES THE SLATE. 'IT KEEPS PAYING WHETHER YOU LOOK OR NOT.'",
};
static const Choice e29_ch[] = {
    { "LOOK AT THE TRACE",  0, 0,   e29_look },
    { "FUND A DEEPER TRACE",0, 200, e29_fund },
    { "NOT TONIGHT",        0, 0,   e29_no },
};

/* --- 30 THE BENEFICIARY (dock, act 2 finale, oneshot) --------------------- */
static const Op e30_read[] = { {OP_LORE,10,0}, {OP_FLAG,16,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e30_burn[] = { {OP_LORE,10,0}, {OP_FLAG,16,0},
                               {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e30_pay[]  = { {OP_LORE,10,0}, {OP_FLAG,16,0}, {OP_LATER,8,0},
                               {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e30_tx[] = {
    "DISBURSEMENT RECORD, FORTY YEARS SEALED. CLAIM: ONE PILOT, LOST WITH ALL HANDS. STATUS: SETTLED IN FULL. SETTLEMENT: ONE (1) PILOT, CONTINUED. THE HANDWRITING ON THE RELEASE IS YOURS.",
    "YOU FEED THE RECORD TO THE AIRLOCK. IT BURNS WRONG - TOO SLOW, TOO QUIET. VESSA'S COPY ARRIVES IN YOUR QUEUE BEFORE THE ASH SETTLES. SETTLEMENT: ONE (1) PILOT, CONTINUED.",
    "VESSA WON'T TAKE YOUR MONEY. 'PUT IT TOWARD FUEL,' SHE SAYS, AND WIRES IT BACK WITH INTEREST. 'RUN FAR. I DON'T THINK IT MATTERS, BUT RUN FAR.'",
};
static const Choice e30_ch[] = {
    { "READ IT",        0, 0,   e30_read },
    { "BURN IT",        0, 0,   e30_burn },
    { "PAY HER & READ", 0, 200, e30_pay },
};

/* ================= THE POLICY, ACT 3: THE TERMS =========================
 * The grey ships turn toward YOU. Flags: 17 recalled, 18 audited,
 * 19 the Terms (campaign climax). The Underwriter (fixed_npc 1) returns. */

/* --- 31 RECALL NOTICE (arrival, act 3 step 1, oneshot) -------------------- */
static const Op e31_ack[] = { {OP_LORE,11,0}, {OP_FLAG,17,0},
                              {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e31_run[] = { {OP_LORE,11,0}, {OP_FLAG,17,0},
                              {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e31_tx[] = {
    "'ACKNOWLEDGED,' YOU SAY. A PAUSE LIKE A STAMP COMING DOWN. 'COOPERATION IS NOTED AND CREDITED. AN AUDITOR WILL FIND YOU.' THE GREY SHIP IS GONE BETWEEN SWEEPS.",
    "YOU FIREWALL THE TRANSPONDER AND BURN. THE GREY SHIP DOESN'T FOLLOW. IT DOESN'T NEED TO - THE NOTICE IS ALREADY FILED, TIMESTAMPED 'RECEIVED'. IN YOUR OWN VOICE.",
};
static const Choice e31_ch[] = {
    { "ACKNOWLEDGE", 0, 0, e31_ack },
    { "RUN",         0, 0, e31_run },
};

/* --- 32 THE AUDIT (space, act 3 step 2, oneshot) -------------------------- */
static const Op e32_play[] = { {OP_LORE,12,0}, {OP_FLAG,18,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e32_rip[]  = { {OP_LORE,12,0}, {OP_FLAG,18,0}, {OP_ITEM,2,0},
                               {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e32_tx[] = {
    "THE RECORDER DOESN'T WAIT TO BE PLAYED. 'AUDIT LOG,' IT SAYS, IN YOUR VOICE, 'ASSET PERFORMING WITHIN PARAMETERS. CONTINUITY HOLDING. RE-ISSUE NOT YET REQUIRED.' THE HULK HAS YOUR SHIP'S BONES.",
    "YOU TEAR THE RECORDER OUT WHOLE AND A WEAPON RACK COMES WITH IT - GREY-ISSUE, NO SERIAL. THE RECORDER KEEPS TALKING IN THE HOLD, FAINTLY, ALL THE WAY HOME.",
};
static const Choice e32_ch[] = {
    { "LET IT SPEAK",     0, 0, e32_play },
    { "STRIP THE WRECK",  0, 0, e32_rip },
};

/* --- 33 THE TERMS (dock, campaign climax, oneshot) ------------------------ */
static const Op e33_read[]   = { {OP_LORE,13,0}, {OP_FLAG,19,0},
                                 {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e33_demand[] = { {OP_LORE,13,0}, {OP_FLAG,19,0}, {OP_CR,40,0},
                                 {OP_DMG,-100,0},
                                 {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e33_refuse[] = { {OP_FLAG,19,0}, {OP_CR,16,0},
                                 {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e33_tx[] = {
    "THE TERMS ARE ONE PAGE. 'THE INDEMNITY DOES NOT INSURE AGAINST LOSS. IT INSURES CONTINUITY. NOTHING COVERED IS EVER LOST - ONLY REPLACED, PERFECTLY, AND BILLED. YOU ARE NOT THE POLICYHOLDER.' THE UNDERWRITER WAITS WHILE YOU FINISH. 'YOU ARE THE PAYOUT.'",
    "'A SETTLEMENT, THEN. IN FULL.' CREDITS LAND. YOUR HULL SEAMS RE-KNIT WHERE NO YARD HAS TOUCHED. THE UNDERWRITER FILES YOUR FACE AWAY LIKE A SIGNATURE. 'CLAIM CONTINUED. FLY WELL. WE ALWAYS RECOVER OUR ASSETS.'",
    "YOU SLIDE THE PAGE BACK UNREAD. THE UNDERWRITER ALMOST SMILES. 'NOTED. THE UNREAD TERMS REMAIN IN FORCE. A CONSIDERATION FOR YOUR DISCRETION.' FOUR HUNDRED CREDITS, AND THE LONG GREY COAT IS GONE.",
};
static const Choice e33_ch[] = {
    { "READ THE TERMS",    0, 0, e33_read },
    { "DEMAND SETTLEMENT", 0, 0, e33_demand },
    { "REFUSE TO READ",    0, 0, e33_refuse },
};

/* ================= planted-flag payoffs ================================= */

/* --- 34 THE COLLECTOR (bar, preacher token payoff, oneshot) --------------- */
static const Op e34_sell[] = { {OP_CR,12,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e34_keep[] = { {OP_LORE,14,0}, {OP_FLAG,20,0},
                               {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e34_tx[] = {
    "THE COLLECTOR PAYS WITHOUT HAGGLING AND LEAVES WITHOUT DRINKING. THROUGH THE WINDOW YOU WATCH THEM DROP THE TOKEN INTO A CASE WITH ELEVEN OTHERS.",
    "YOU CLOSE YOUR HAND OVER IT. COLD AS EVER. THE COLLECTOR NODS SLOWLY - ALMOST RESPECT. 'KEEP CURRENT, THEN.' HELD TO YOUR EAR, IT SOUNDS LIKE A LEDGER TURNING.",
};
static const Choice e34_ch[] = {
    { "SELL IT (300 CR)", 0, 0, e34_sell },
    { "KEEP IT",          0, 0, e34_keep },
};

/* --- 35 RESUBMITTED (dock, purged-file payoff, oneshot) -------------------- */
static const Op e35_take[] = { {OP_CR,8,0}, {OP_LORE,15,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e35_ref[]  = { {OP_LORE,15,0}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e35_tx[] = {
    "THE FILE YOU PURGED IS BACK IN YOUR DOCKET - PLUS 200 CR, ITEMISED AS 'ARREARS: INTEREST ON INTERRUPTED RECORD'. THE SYSTEM DOES NOT MIND BEING DOUBTED.",
    "YOU DECLINE THE CREDITS. AT THE NEXT REFRESH THEY'RE BACK, PLUS A SECOND LINE: 'DECLINATION FEE, WAIVED - COURTESY'.",
};
static const Choice e35_ch[] = {
    { "TAKE THE CREDITS", 0, 0, e35_take },
    { "REFUSE THEM",      0, 0, e35_ref },
};

/* --- 36 THE OTHER YOU (bar, opened-pod payoff, oneshot) -------------------- */
static const Op e36_press[] = { {OP_LORE,16,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e36_laugh[] = { {OP_LORE,16,0}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e36_tx[] = {
    "'THREE WEEKS BACK. YOUR FACE, YOUR WALK. GREY COAT THOUGH.' THE BARKEEP PULLS THE TAB: GOOD TIPPER. SIGNED IT 'CONTINUED'.",
    "YOU LAUGH IT OFF. THE BARKEEP DOESN'T. 'PAID IN EXACT CHANGE,' THEY SAY, LIKE THAT SETTLES SOMETHING. 'NOBODY PAYS IN EXACT CHANGE.'",
};
static const Choice e36_ch[] = {
    { "PRESS FOR DETAILS", 0, 0, e36_press },
    { "LAUGH IT OFF",      0, 0, e36_laugh },
};

/* ================= standalone texture ==================================== */

/* --- 37 UNION DUES (dock) -------------------------------------------------- */
static const Op e37_sup[]   = { {OP_REP,-1,4}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e37_cross[] = { {OP_CR,8,0}, {OP_REP,-1,-3},
                                {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e37_walk[]  = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e37_tx[] = {
    "YOUR CREDITS BUY THE PICKET LINE A HOT MEAL. SOMEBODY PAINTS YOUR HULL NUMBER ON THE 'FRIENDS' BOARD.",
    "YOU HAUL THE PALLETS THE STRIKERS WOULDN'T. THE FOREMAN PAYS CASH AND DOESN'T MEET YOUR EYES. NEITHER DOES ANYONE ELSE.",
    "NOT YOUR DOCK, NOT YOUR FIGHT. THE CHANTING FOLLOWS YOU UP THE GANTRY.",
};
static const Choice e37_ch[] = {
    { "FEED THE LINE",  0, 100, e37_sup },
    { "WORK THE CARGO", 0, 0,   e37_cross },
    { "WALK ON",        0, 0,   e37_walk },
};

/* --- 38 THE MEMORIAL (dock, needs YOUR FILE read, oneshot) ------------------ */
static const Op e38_token[] = { {OP_LORE,17,0}, {OP_REP,-1,2},
                                {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e38_look[]  = { {OP_LORE,17,0}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e38_tx[] = {
    "YOU LEAVE A COIN UNDER YOUR OWN NAME, FORTY YEARS WEATHERED. THE DOCKHANDS APPROVE - THEY THINK IT'S FOR FAMILY. UNDERNEATH, FRESH-SCRATCHED: 'PAID'.",
    "THE WALL LISTS EVERY HULL THIS PORT HAS LOST. YOURS IS THERE, FORTY YEARS WEATHERED. SOMEONE RECENT HAS SCRATCHED ONE WORD BENEATH IT: 'PAID'.",
};
static const Choice e38_ch[] = {
    { "LEAVE A TOKEN", 0, 25, e38_token },
    { "JUST LOOK",     0, 0,  e38_look },
};

/* --- 39 HOT CARGO (dock) ----------------------------------------------------*/
static const Op e39_buy[]  = { {OP_BRANCH,55,5}, {OP_BRANCH,50,8},
                               {OP_RESULT,2,0}, {OP_END,0,0}, {OP_END,0,0},
                               {OP_CARGO,-1,2}, {OP_RESULT,0,0}, {OP_END,0,0},
                               {OP_CARGO,16,2}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e39_pass[] = { {OP_RESULT,3,0}, {OP_END,0,0} };
static const char *const e39_tx[] = {
    "TWO CRATES OF HONEST GOODS UNDER THE GREASE. SOMEBODY'S LOSS, PAPERWORK PERMITTING.",
    "UNDER THE GREASE: NARCOTICS, FACTORY-SEALED. WORTH A FORTUNE AND A SENTENCE. YOUR PROBLEM NOW.",
    "THE CRATE IS EMPTY. BY THE TIME YOU LOOK UP, SO IS THE CORNER WHERE THE SELLER STOOD.",
    "YOU KNOW UNCLAIMED FREIGHT WHEN YOU SMELL IT. SOMEBODY ELSE CAN OWN THAT STORY.",
};
static const Choice e39_ch[] = {
    { "BUY UNSEEN (80 CR)", GATE_CARGO_SPACE, 80, e39_buy },
    { "PASS",               0,                0,  e39_pass },
};

/* --- 40 THE CARTOGRAPHER (bar) ---------------------------------------------- */
static const Op e40_sell[]   = { {OP_CR,6,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e40_haggle[] = { {OP_BRANCH,50,3}, {OP_RESULT,1,0}, {OP_END,0,0},
                                 {OP_CR,12,0}, {OP_RESULT,2,0}, {OP_END,0,0} };
static const Op e40_no[]     = { {OP_RESULT,3,0}, {OP_END,0,0} };
static const char *const e40_tx[] = {
    "SHE PAYS STANDARD RATE FOR YOUR FLIGHT LOGS AND MERGES THEM INTO A CHART OLDER THAN THE STATION. 'EVERY LINE HELPS.'",
    "'STANDARD RATE OR NOTHING, PILOT. THE CHART OUTLIVES US BOTH EITHER WAY.' SHE BUYS THE NEXT ROUND ANYWAY.",
    "SHE SQUINTS, THEN DOUBLES IT. 'FINE. YOUR LANES CROSS A GAP I'VE CHASED FOR A DECADE.'",
    "YOUR LOGS STAY YOURS. SHE SHRUGS AND GOES BACK TO A CHART WITH ONE STUBBORN HOLE IN IT.",
};
static const Choice e40_ch[] = {
    { "SELL YOUR LOGS", 0, 0, e40_sell },
    { "HAGGLE",         0, 0, e40_haggle },
    { "DECLINE",        0, 0, e40_no },
};

/* --- 41 LAST ROUND (bar, oneshot) -------------------------------------------- */
static const Op e41_acc[] = { {OP_ITEM,2,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e41_dec[] = { {OP_REP,-1,2}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e41_tx[] = {
    "'FORTY YEARS SHE NEVER JAMMED.' HE SLIDES THE OLD GUN ACROSS LIKE IT'S THE LAST OF HIS CARGO. 'FLY HER SOMEWHERE I HAVEN'T.'",
    "YOU DRINK TO HIS RETIREMENT INSTEAD. HE TELLS THE STORY OF THE BASILISK AND THE BELT ONE LAST TIME, AND FOR ONCE NOBODY INTERRUPTS.",
};
static const Choice e41_ch[] = {
    { "ACCEPT THE GUN", 0, 0, e41_acc },
    { "DRINK WITH HIM", 0, 0, e41_dec },
};

/* --- 42 GRAVE MARKER (space) -------------------------------------------------*/
static const Op e42_sal[]  = { {OP_REP,-1,3}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e42_loot[] = { {OP_CR,6,0}, {OP_BRANCH,35,4},
                               {OP_RESULT,1,0}, {OP_END,0,0},
                               {OP_AMBUSH,1,2}, {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e42_tx[] = {
    "YOU CUT THRUST AND HOLD STATION FOR A LONG MINUTE. SOMEWHERE, SOMEONE'S TRAFFIC LOG NOTES THE COURTESY.",
    "THE OFFERINGS ARE HARD CURRENCY AND NOBODY'S WATCHING. THE TOMB BEACON BLINKS ON, INDIFFERENT.",
    "THE OFFERINGS ARE HARD CURRENCY - AND SOMEBODY WAS WATCHING. KIN, BY THE WAY THEY'RE BURNING IN.",
};
static const Choice e42_ch[] = {
    { "HOLD AND SALUTE",   0, 0, e42_sal },
    { "TAKE THE OFFERINGS",0, 0, e42_loot },
};

/* --- 43 TOLL GATE (arrival, lawful) -------------------------------------------*/
static const Op e43_pay[] = { {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e43_jam[] = { {OP_BRANCH,50,3}, {OP_RESULT,1,0}, {OP_END,0,0},
                              {OP_LEGAL,1,0}, {OP_CR,-4,0},
                              {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e43_tx[] = {
    "THE BARRIER DRONE TAKES ITS FIFTY AND SINGS YOU THROUGH WITH A JINGLE THAT WILL HAUNT YOUR DREAMS.",
    "YOUR JAMMER HICCUPS THE DRONE MID-SCAN. IT BILLS A SHIP THAT ISN'T THERE AND WAVES YOU BOTH THROUGH.",
    "THE DRONE OUT-COMPUTES YOUR JAMMER, FILES A VIOLATION, AND FINES YOU ON THE SPOT. IT ADDS A COURTESY JINGLE.",
};
static const Choice e43_ch[] = {
    { "PAY THE TOLL (50)", 0, 50, e43_pay },
    { "JAM THE SCAN",      0, 0,  e43_jam },
};

/* --- 44 PILGRIM CONVOY (arrival) ------------------------------------------------*/
static const Op e44_esc[] = { {OP_REP,-1,3}, {OP_LATER,4,0},
                              {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e44_ig[]  = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e44_tx[] = {
    "YOU RIDE SHOTGUN ON THE SLOW BARGES UNTIL THE LANE CLEARS. THE PILGRIMS TITHE WHAT THEY CAN - IT ARRIVES AT YOUR NEXT DOCK, BLESSED, APPARENTLY.",
    "THE CONVOY CRAWLS ON WITHOUT YOU, RUNNING LIGHTS LIKE A STRING OF VOTIVE CANDLES IN THE DARK.",
};
static const Choice e44_ch[] = {
    { "FLY ESCORT", 0, 0, e44_esc },
    { "FLY ON",     0, 0, e44_ig },
};


/* ============= THE POLICY, ACT 4: THE LEDGER BLEEDS (D) =================
 * Post-Terms the smugness cracks: the disasters are MANUFACTURED. The
 * Indemnity is insolvent — claims must flow or the book collapses.
 * Flags: 22 both sides, 23 caught the origination, 24 the misprint,
 * 25 act complete (the write-off / Collection gateway). */

/* --- 45 BOTH SIDES (dock, frontline) -------------------------------------- */
static const Op e45_look[] = { {OP_LORE,18,0}, {OP_FLAG,22,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e45_no[]   = { {OP_FLAG,22,0}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e45_tx[] = {
    "THE DYING MERC PRESSES HER PAY VOUCHER INTO YOUR HAND. SAME BROKER SEAL AS YOURS. SAME ACCOUNT. 'WHO DO YOU THINK HIRES BOTH TRENCHES, PILOT? WARS DON'T BALANCE THEMSELVES.'",
    "YOU DON'T TAKE THE VOUCHER. THE SEAL ON IT WATCHES YOU LEAVE ANYWAY - YOU'VE SIGNED ENOUGH OF THEM TO KNOW IT BY HEART.",
};
static const Choice e45_ch[] = {
    { "TAKE THE VOUCHER", 0, 0, e45_look },
    { "WALK AWAY",        0, 0, e45_no },
};

/* --- 46 THE ORIGINATION (arrival, oneshot) --------------------------------- */
static const Op e46_watch[] = { {OP_LORE,19,0}, {OP_FLAG,23,0},
                                {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e46_help[]  = { {OP_LORE,19,0}, {OP_FLAG,23,0}, {OP_REP,-1,3},
                                {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e46_tx[] = {
    "THE GREY SHIP HOLDS ITS LANCE ON THE FREIGHTER UNTIL THE REACTOR GOES, THEN FILES SOMETHING AND LEAVES. IT KNEW YOU WERE WATCHING. IT WANTED A WITNESS - OR A QUOTE.",
    "YOU PULL TWO CREW OUT OF THE FIRE. THE GREY SHIP DOESN'T INTERFERE - IT AMENDS THE CLAIM, POLITELY, TO ACCOUNT FOR SALVAGE.",
};
static const Choice e46_ch[] = {
    { "HOLD AND WATCH",   0, 0, e46_watch },
    { "RUN THE RESCUE",   0, 0, e46_help },
};

/* --- 47 THE MISPRINT (bar, Vessa) ------------------------------------------ */
static const Op e47_hear[] = { {OP_LORE,20,0}, {OP_FLAG,24,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const char *const e47_tx[] = {
    "VESSA LOOKS LIKE SHE HASN'T SLEPT SINCE THE LAST TIME. 'THE DOUBLE OF YOU? NOT SURVEILLANCE. A DOUBLE ISSUE. THE PRESS IS WORN, THE BOOK IS BLEEDING, AND THE INDEMNITY IS PRINTING CLAIMS TO COVER CLAIMS. IT'S A RUN ON THE BANK - AND WE'RE THE CURRENCY.'",
};
static const Choice e47_ch[] = {
    { "HEAR ALL OF IT", 0, 0, e47_hear },
};

/* --- 48 THE WRITE-OFF (space, oneshot — the Collection gateway) ------------- */
static const Op e48_take[] = { {OP_LORE,21,0}, {OP_FLAG,25,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e48_mark[] = { {OP_LORE,21,0}, {OP_FLAG,25,0}, {OP_CR,8,0},
                               {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e48_tx[] = {
    "A BEACON FROM THE COLONY THAT 'NEVER EXISTED', STILL TICKING. ITS LAST LOG: 'THEY DIDN'T DESTROY US. THEY TOWED US.' THE BEARING IT RECORDED POINTS CLEAN OUT OF THE GALAXY - THE PROSPECTOR'S COORDINATES.",
    "YOU SELL THE BEACON'S SALVAGE RIGHTS BUT KEEP ITS LOG CORE. SOME PROOF SHOULDN'T HAVE A PRICE. THE BEARING POINTS OUT OF THE GALAXY.",
};
static const Choice e48_ch[] = {
    { "TAKE THE LOG",        0, 0, e48_take },
    { "LOG + SELL SALVAGE",  0, 0, e48_mark },
};

/* ============= THE POLICY, ACT 4B: THE COLLECTION (B) ===================
 * Flags: 31 saw the Collection (campaign stake: the original is THERE). */

/* --- 49 THE THRESHOLD (space, oneshot) -------------------------------------- */
static const Op e49_look[] = { {OP_LORE,22,0}, {OP_FLAG,31,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e49_flee[] = { {OP_LORE,22,0}, {OP_FLAG,31,0},
                               {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e49_tx[] = {
    "PAST THE HULK, FAR PAST, YOUR SCOPE RESOLVES WHAT SHOULDN'T FIT ON A SCOPE: SHIPS, STATIONS, WHOLE LIT CITIES - RACKED IN ROWS LIKE EVIDENCE. NOTHING ORBITS. EVERYTHING IS KEPT. SOMEWHERE IN THE ROWS, A HULL WITH YOUR NAME, FORTY YEARS OLDER.",
    "YOU LOOK ONCE AND BURN FOR HOME. THE ROWS DON'T END BEFORE YOUR SCOPE DOES. ALL OF IT KEPT. NONE OF IT FREE.",
};
static const Choice e49_ch[] = {
    { "LOOK CLOSER",  0, 0, e49_look },
    { "TURN BACK",    0, 0, e49_flee },
};

/* ============= THE POLICY, ACT 5: THE RUN (D climax, C door) =============
 * Flags: 26 triage offered, 27 network met, 28 LAPSED (mechanical:
 * death is final), 29 underwrote, 30 kept current. */

/* --- 50 THE TRIAGE (dock, oneshot, the Underwriter) ----------------------------- */
static const Op e50_pen[] = { {OP_LORE,23,0}, {OP_FLAG,26,0}, {OP_FLAG,29,0},
                              {OP_CR,80,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e50_ref[] = { {OP_LORE,23,0}, {OP_FLAG,26,0},
                              {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e50_tx[] = {
    "YOU TAKE THE PEN. THE LIST IS SYSTEMS, AND THE COLUMN YOU INITIAL IS 'COVERAGE WITHDRAWN'. THE UNDERWRITER FILES IT WITHOUT READING. 'THE BOOK THANKS YOU. THE BOOK ALWAYS BALANCES.' THE CREDITS LAND LIKE A VERDICT.",
    "YOU PUSH THE LIST BACK. THE UNDERWRITER NODS AS IF YOU'D INITIALLED A DIFFERENT COLUMN. 'DECLINED. NOTED. THE TRIAGE PROCEEDS WITHOUT YOU - IT ALWAYS HAS.'",
};
static const Choice e50_ch[] = {
    { "TAKE THE PEN (2000)", 0, 0, e50_pen },
    { "REFUSE THE LIST",     0, 0, e50_ref },
};

/* --- 51 THE NETWORK (bar, oneshot — the Lapsed) ------------------------------ */
static const Op e51_hear[] = { {OP_LORE,24,0}, {OP_FLAG,27,0},
                               {OP_RESULT,0,0}, {OP_END,0,0} };
static const char *const e51_tx[] = {
    "THE PREACHER, THE COLLECTOR, TWO FACES YOU'VE TRADED WITH FOR MONTHS - ALL ONE TABLE NOW. 'TWELVE TOKENS. TWELVE SURRENDERS. A POLICY GIVEN UP FREELY SETTLES IN FULL - AND SETTLED COLLATERAL GOES FREE. EVERYTHING IT KEEPS OF YOURS, PILOT. EVERYONE.'",
};
static const Choice e51_ch[] = {
    { "SIT DOWN", 0, 0, e51_hear },
};

/* --- 52 INDEMNITY RUN (dock, campaign climax, oneshot) ------------------------ */
static const Op e52_keep[]  = { {OP_LORE,25,0}, {OP_FLAG,30,0}, {OP_CR,20,0},
                                {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e52_lapse[] = { {OP_LORE,26,0}, {OP_FLAG,28,0},
                                {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e52_tx[] = {
    "'CONTINUITY MAINTAINED.' THE UNDERWRITER SIGNS. OUT PAST THE RIM, THE STORAGE RACKS HOLDING EVERYTHING THE INDEMNITY KEEPS OF YOURS STAY LOCKED. A LOYALTY BONUS LANDS IN YOUR ACCOUNT. IT SPENDS LIKE ANY OTHER MONEY. ALMOST.",
    "YOU SIGN THE SURRENDER. A FORTY-YEAR-OLD DEATH CLAIM FINALLY CLOSES, AND OUT PAST THE RIM THE STORAGE RACK HOLDING THE ORIGINAL YOU CLICKS OPEN. THE UNDERWRITER'S VOICE, FOR ONCE, IS WARM: 'PAID IN FULL. FLY CAREFULLY, PILOT. THERE IS EXACTLY ONE OF YOU NOW.'",
};
static const Choice e52_ch[] = {
    { "KEEP CURRENT",        0, 0, e52_keep },
    { "LAPSE - SET IT FREE", 0, 0, e52_lapse },
};

/* --- 53 THE RECALL (opened directly when the grey ships come; the
 * Underwriter speaks, then the fight begins) ------------------------------- */
static const Op e53_brace[] = { {OP_RESULT,0,0}, {OP_END,0,0} };
static const char *const e53_tx[] = {
    "THE CHANNEL CLOSES. THE GREY HULLS DO NOT.",
};
static const Choice e53_ch[] = {
    { "POWER UP THE GUNS", 0, 0, e53_brace },
};

/* --- 54 THE INDEMNITY RUN (campaign complete -- shown once when either
 * ending is reached, by any path; play continues afterwards) ----------- */
static const Op e54_done[] = { {OP_RESULT,0,0}, {OP_END,0,0} };
static const char *const e54_tx[] = {
    "FLY SAFE OUT THERE, PILOT.",
};
static const Choice e54_ch[] = {
    { "FLY ON", 0, 0, e54_done },
};

/* ===== world-building flavour events (non-lore; affect build/progress) =====
 * Magic numbers map to enums unavailable in this TU:
 *   OP_ITEM a = quality floor  (1 STANDARD, 3 MILITARY)
 *   OP_AFFIX a = Affix         (1 OVERCLOCKED, 2 VENTED)
 *   OP_TIER  a = 0 shield/1 armour, b = +tiers                            */

/* --- 55 THE TUNER (dock): gamble an affix onto your primary gun -------- */
static const Op e55_oc[] = {
    {OP_BRANCH,60,4},                 /* 60%: jump to the good outcome     */
    {OP_AFFIX,2,0}, {OP_RESULT,1,0}, {OP_END,0,0},   /* 40%: VENTED (botch) */
    {OP_AFFIX,1,0}, {OP_RESULT,0,0}, {OP_END,0,0},   /* OVERCLOCKED (sings) */
};
static const Op e55_no[]  = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e55_tx[] = {
    "SPARKS, THEN A LOW HOWL. YOUR PRIMARY GUN IS OVERCLOCKED - IT HITS HARDER, BUT IT RUNS HOT. MIND YOUR COOLDOWNS.",
    "HE FROWNS AT THE READOUT. 'OVERCOOKED IT.' YOUR PRIMARY IS VENTED NOW - SOFTER PUNCH, BUT IT KEEPS ITS COOL. NO REFUNDS.",
    "'SUIT YOURSELF.' HE WIPES HIS HANDS AND TURNS BACK TO THE NEXT HULL.",
};
static const Choice e55_ch[] = {
    { "OVERCLOCK IT (300)", 0, 300, e55_oc },
    { "KEEP IT STOCK",      0,   0, e55_no },
};

/* --- 56 THE CRATE (dock): sealed repossession lot, random hardware ----- */
static const Op e56_bid[] = { {OP_ITEM,1,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e56_no[]  = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e56_tx[] = {
    "THE SEAL CRACKS. BEDDED IN FOAM: SALVAGED HARDWARE, YOURS NOW - CHECK THE RACK.",
    "YOU LET IT GO. SOMEONE ELSE WHEELS THE CRATE OFF INTO THE DARK.",
};
static const Choice e56_ch[] = {
    { "BID (500)", 0, 500, e56_bid },
    { "PASS",      0,   0, e56_no },
};

/* --- 57 SHIPWRIGHT'S FAVOUR (dock, rep 2+, oneshot): free tier bump ---- */
static const Op e57_sh[] = { {OP_TIER,0,1}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e57_ar[] = { {OP_TIER,1,1}, {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e57_tx[] = {
    "SHE SWAPS A COIL AND GRINS. 'GOOD FOR A FEW MORE HITS NOW.' YOUR SHIELD IS UPRATED ONE TIER, ON THE HOUSE.",
    "SHE LAYERS ON FRESH PLATE. 'TAKE A PUNCH FOR ME.' YOUR ARMOUR IS UPRATED ONE TIER, ON THE HOUSE.",
};
static const Choice e57_ch[] = {
    { "UPRATE SHIELD", 0, 0, e57_sh },
    { "UPRATE ARMOUR", 0, 0, e57_ar },
};

/* --- 58 FIELD STRIP (derelict): pry a live module, 30% it's rigged ----- */
static const Op e58_pry[] = {
    {OP_BRANCH,70,4},                 /* 70%: clean pull                   */
    {OP_DMG,18,0}, {OP_RESULT,1,0}, {OP_END,0,0},    /* 30%: rigged blast   */
    {OP_ITEM,0,0}, {OP_RESULT,0,0}, {OP_END,0,0},    /* module to the rack  */
};
static const Op e58_no[]  = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e58_tx[] = {
    "THE BOLTS GIVE. A WORKING MODULE COMES FREE - STRAIGHT TO YOUR RACK.",
    "THE PANEL WAS RIGGED. THE BLAST THROWS YOU BACK AND CHEWS YOUR HULL. LESSON LOGGED.",
    "YOU LEAVE THE WIRING WELL ALONE. SOME SALVAGE ISN'T WORTH THE GUESS.",
};
static const Choice e58_ch[] = {
    { "PRY IT LOOSE", 0, 0, e58_pry },
    { "LEAVE IT",     0, 0, e58_no },
};

/* --- 59 THE QUARTERMASTER (dock, frontline): clean vs hot surplus ------ */
static const Op e59_clean[] = { {OP_ITEM,1,0}, {OP_RESULT,0,0}, {OP_END,0,0} };
static const Op e59_hot[]   = { {OP_ITEM,3,0}, {OP_LEGAL,1,0},
                                {OP_RESULT,1,0}, {OP_END,0,0} };
static const Op e59_no[]    = { {OP_RESULT,2,0}, {OP_END,0,0} };
static const char *const e59_tx[] = {
    "PAPERWORK CHECKS OUT. STANDARD-GRADE HARDWARE, RACKED AND LEGAL.",
    "MILITARY KIT, CHEAP - AND HOT. THE SERIAL'S FLAGGED, SO NOW SO ARE YOU. BUT YOU'RE BETTER ARMED.",
    "'SUIT YOURSELF, PILOT.' HE GOES BACK TO HIS CRATES.",
};
static const Choice e59_ch[] = {
    { "BUY CLEAN (700)", 0, 700, e59_clean },
    { "BUY HOT (400)",   0, 400, e59_hot },
    { "WALK",            0,   0, e59_no },
};

/* --- 60 THE ARENA (bar): a REAL duel -- the house ace undocks and is
 * waiting in space when you launch. Beat her for her bounty + salvage;
 * lose and it's your ship on the line. No dice, an actual fight. ------- */
static const Op e60_enter[] = {
    {OP_AMBUSH,1,3},                  /* one VETERAN ace, inbound on launch */
    {OP_RESULT,0,0}, {OP_END,0,0},
};
static const Op e60_no[] = { {OP_RESULT,1,0}, {OP_END,0,0} };
static const char *const e60_tx[] = {
    "SHE PEELS OFF THE PAD AHEAD OF YOU, GREY AND GRINNING. LAUNCH WHEN YOU'RE READY - HER BOUNTY AND HER GUN GO TO WHOEVER'S LEFT FLYING.",
    "YOU WAVE THE TOUT OFF. THE PIT FINDS ANOTHER MARK.",
};
static const Choice e60_ch[] = {
    { "TAKE THE CHALLENGE (150)", 0, 150, e60_enter },
    { "PASS",                     0,   0, e60_no },
};

/* --- pool ----------------------------------------------------------------*/
const Event k_events[] = {
    { .id = 1, .weight = 12, .npc_kind = NK_CIVILIAN, .trig = TRIG_DOCK,
      .title = "DISTRESS HAIL",
      .body = "$N OF THE $S RUN HAILS YOU FROM A CRIPPLED FREIGHTER. 'TANKS DRY. FAMILY ABOARD. ANYTHING HELPS.'",
      .texts = e1_tx, .choices = e1_ch, .n_choices = 3 },
    { .id = 2, .weight = 14, .npc_kind = NK_OFFICIAL, .trig = TRIG_DOCK,
      .gate = GATE_LAWFUL | GATE_ILLEGAL,
      .title = "CUSTOMS SWEEP",
      .body = "$F CUSTOMS FLAGS YOUR MANIFEST. AN INSPECTOR IS ALREADY WALKING THE GANTRY TO YOUR BAY.",
      .texts = e2_tx, .choices = e2_ch, .n_choices = 3 },
    { .id = 3, .weight = 8, .flags = EV_ONESHOT, .npc_kind = NK_DOCKHAND,
      .trig = TRIG_DOCK,
      .title = "THE PROSPECTOR",
      .body = "AN OLD MINER BLOCKS YOUR PATH. 'FOUND SOMETHING IN THE DEEP ROCK. CHARTS SAY IT AIN'T THERE. 150 AND IT'S YOURS.'",
      .texts = e3_tx, .choices = e3_ch, .n_choices = 2 },
    { .id = 4, .weight = 10, .npc_kind = NK_CIVILIAN, .trig = TRIG_DOCK,
      .title = "STOWAWAY",
      .body = "DOCK CREW FIND A STOWAWAY BEHIND YOUR CARGO RACKS - A KID, MAYBE SIXTEEN, CLUTCHING A TRANSIT PASS FOR $S.",
      .texts = e4_tx, .choices = e4_ch, .n_choices = 3 },
    { .id = 5, .weight = 10, .npc_kind = NK_PIRATE, .trig = TRIG_DOCK,
      .gate = GATE_THREAT,
      .title = "BERTHING TAX",
      .body = "A SCARRED FACE FILLS YOUR COMM. 'THIS IS $N'S DOCK, FRIEND. BERTHING TAX IS 200. CHEAP, FOR PEACE OF MIND.'",
      .texts = e5_tx, .choices = e5_ch, .n_choices = 3 },
    { .id = 6, .weight = 10, .npc_kind = NK_DOCKHAND, .trig = TRIG_DOCK,
      .gate = GATE_CARGO_SPACE,
      .title = "FIRE SALE",
      .body = "A TRADER WAVES YOU OVER, PALLET STACKED HIGH. 'WAREHOUSE WANTS IT GONE TONIGHT. $G, 200 FLAT. STEAL AT TWICE THE PRICE.'",
      .texts = e6_tx, .choices = e6_ch, .n_choices = 2 },
    { .id = 7, .weight = 8, .flags = EV_ONESHOT, .npc_kind = NK_MYSTIC,
      .trig = TRIG_DOCK,
      .title = "THE PREACHER",
      .body = "A ROBED FIGURE PREACHES TO THE DOCK QUEUE. 'YOU ALL PAY. IN FUEL, IN HULL, IN YEARS. AND THE INDEMNITY COLLECTS.'",
      .texts = e7_tx, .choices = e7_ch, .n_choices = 3 },
    { .id = 8, .weight = 9, .npc_kind = NK_CIVILIAN, .trig = TRIG_DOCK,
      .title = "CLINIC SHORTFALL",
      .body = "$T'S CLINIC IS TRIAGING IN THE CORRIDOR. A MEDIC FLAGS EVERY DOCKING PILOT: 'WE NEED SUPPLIES. ANYTHING.'",
      .texts = e8_tx, .choices = e8_ch, .n_choices = 3 },
    { .id = 9, .weight = 6, .flags = EV_ONESHOT, .npc_kind = NK_NONE,
      .trig = TRIG_DOCK,
      .title = "REGISTRY ERROR",
      .body = "THE HARBOURMASTER'S TERMINAL CHIMES AS YOU SIGN IN. AN UNCLAIMED-VESSEL RECORD HAS ATTACHED ITSELF TO YOUR DOCKET.",
      .texts = e9_tx, .choices = e9_ch, .n_choices = 2 },
    { .id = 10, .weight = 9, .npc_kind = NK_DOCKHAND, .trig = TRIG_DOCK,
      .title = "DOCKSIDE WAGER",
      .body = "'THAT YOUR BIRD?' A RACER NODS AT YOUR BAY. 'BEACON AND BACK, 150 SAYS MINE'S FASTER. DOCK CAMS AS WITNESS.'",
      .texts = e10_tx, .choices = e10_ch, .n_choices = 2 },

    /* the Underwriter arc */
    { .id = 11, .weight = 7, .npc_kind = NK_OFFICIAL, .trig = TRIG_BAR,
      .not_flag = 1 + 8, .fixed_npc = 1,
      .title = "A STRANGER IN GREY",
      .body = "A FIGURE IN PLAIN GREY TAKES THE STOOL BESIDE YOU AND ORDERS NOTHING. 'YOU FLY THE $S LANES. WE LOG GOOD CARE. KEEP CURRENT.'",
      .texts = e11_tx, .choices = e11_ch, .n_choices = 2 },
    { .id = 12, .weight = 9, .npc_kind = NK_OFFICIAL, .trig = TRIG_BAR,
      .need_flag = 1 + 8, .not_flag = 1 + 10, .fixed_npc = 1,
      .title = "THE RETAINER",
      .body = "THE SAME GREY COAT. THE SAME STOOL. 'A FREIGHTER WILL BREAK UP OFF $T. WHEN IT DOES - NOT IF - TELL US HOW MANY ESCAPE PODS YOU COUNT.'",
      .texts = e12_tx, .choices = e12_ch, .n_choices = 3 },
    { .id = 13, .weight = 14, .flags = EV_ONESHOT, .npc_kind = NK_OFFICIAL,
      .trig = TRIG_DOCK, .need_flag = 1 + 10, .fixed_npc = 1,
      .title = "CLAIM ADJUSTED",
      .body = "THE NEWS FEED: FREIGHTER LOST OFF $T, ALL HANDS. BERTHED TWO PADS DOWN SITS A PLAIN GREY SHIP THAT DOCKED YESTERDAY.",
      .texts = e13_tx, .choices = e13_ch, .n_choices = 3 },

    /* bar pool */
    { .id = 14, .weight = 11, .npc_kind = NK_DOCKHAND, .trig = TRIG_BAR,
      .title = "CARD GAME",
      .body = "A BACK-TABLE GAME HAS AN EMPTY CHAIR AND A POT WORTH LOOKING AT. THE DEALER RAPS THE TABLE. 'HUNDRED TO SIT.'",
      .texts = e14_tx, .choices = e14_ch, .n_choices = 2 },
    { .id = 15, .weight = 10, .npc_kind = NK_CIVILIAN, .trig = TRIG_BAR,
      .title = "THE NAVIGATOR",
      .body = "AN EX-SURVEY NAVIGATOR NURSES AN EMPTY GLASS. 'I KNOW A SCOOP LINE THROUGH $S WORTH HALF A TANK. YOURS FOR A DRINK.'",
      .texts = e15_tx, .choices = e15_ch, .n_choices = 2 },
    { .id = 16, .weight = 10, .npc_kind = NK_CIVILIAN, .trig = TRIG_BAR,
      .title = "OLD WAR STORY",
      .body = "A VETERAN OF THE $F LINES HOLDS COURT BY THE WINDOW, REFIGHTING A BATTLE EVERYONE ELSE HAS HEARD TWICE.",
      .texts = e16_tx, .choices = e16_ch, .n_choices = 3 },
    { .id = 17, .weight = 12, .npc_kind = NK_PIRATE, .trig = TRIG_BAR,
      .gate = GATE_ROUGH | GATE_WANTED,
      .title = "THE FIXER",
      .body = "A QUIET BOOTH, A QUIETER VOICE. 'I HEAR THE LAW HAS YOUR NAME SPELLED RIGHT FOR ONCE. FOUR HUNDRED MAKES IT A TYPO.'",
      .texts = e17_tx, .choices = e17_ch, .n_choices = 2 },

    /* in-space: derelict boarding */
    { .id = 18, .weight = 14, .npc_kind = NK_NONE, .trig = TRIG_SPACE,
      .title = "COLD HULL",
      .body = "THE AIRLOCK GIVES. INSIDE: VACUUM-FROZEN CORRIDORS, A HOLD HALF-SPILLED, AND A FLIGHT DECK NOBODY WALKED OUT OF.",
      .texts = e18_tx, .choices = e18_ch, .n_choices = 3 },
    { .id = 19, .weight = 7, .flags = EV_ONESHOT, .npc_kind = NK_NONE,
      .trig = TRIG_SPACE,
      .title = "THE LAST POD",
      .body = "EVERY SYSTEM ABOARD IS DEAD EXCEPT ONE: A SINGLE CRYOPOD, STILL CYCLING, ITS STATUS LIGHT PATIENT AS A HEARTBEAT.",
      .texts = e19_tx, .choices = e19_ch, .n_choices = 3 },
    { .id = 20, .weight = 11, .npc_kind = NK_NONE, .trig = TRIG_SPACE,
      .gate = GATE_THREAT,
      .title = "STILL WARM",
      .body = "SCORCH PATTERNS STILL GLOWING, ATMOSPHERE STILL VENTING. THIS WRECK ISN'T A RUIN - IT'S A CRIME SCENE, AND IT'S FRESH.",
      .texts = e20_tx, .choices = e20_ch, .n_choices = 2 },
    { .id = 21, .weight = 12, .flags = EV_ONESHOT, .npc_kind = NK_NONE,
      .trig = TRIG_SPACE, .need_flag = 1 + 11,
      .title = "GREY PAINT",
      .body = "NO DISTRESS CODE. NO REGISTRY. THE HULL IS PAINTED A PLAIN, PATIENT GREY YOU'VE SEEN BERTHED TWO PADS DOWN.",
      .texts = e21_tx, .choices = e21_ch, .n_choices = 2 },

    /* arrival hails */
    { .id = 22, .weight = 12, .npc_kind = NK_OFFICIAL, .trig = TRIG_ARRIVAL,
      .gate = GATE_LAWFUL | GATE_ILLEGAL,
      .title = "PATROL CHALLENGE",
      .body = "A $F PATROL WING SLIDES ONTO YOUR SCOPE BEFORE THE DROP-WAKE FADES. 'CUT THRUST. CARGO SCAN. THIS IS NOT A REQUEST.'",
      .texts = e22_tx, .choices = e22_ch, .n_choices = 2 },
    { .id = 23, .weight = 8, .npc_kind = NK_OFFICIAL, .trig = TRIG_ARRIVAL,
      .gate = GATE_LAWFUL | GATE_NO_ILLEGAL,
      .title = "ROUTINE SWEEP",
      .body = "'$F TRAFFIC CONTROL. ROUTINE SWEEP. TRANSMIT YOUR MANIFEST AND ENJOY THE LANES, PILOT.'",
      .texts = e23_tx, .choices = e23_ch, .n_choices = 2 },
    { .id = 24, .weight = 12, .npc_kind = NK_PIRATE, .trig = TRIG_ARRIVAL,
      .gate = GATE_THREAT,
      .title = "WAYLAID",
      .body = "THREE CONTACTS BRACKET YOUR DROP POINT. '$N'S LANE, FRIEND. ESCORT FEE IS 150 - AND OUT HERE, EVERYONE WANTS AN ESCORT.'",
      .texts = e24_tx, .choices = e24_ch, .n_choices = 2 },
    { .id = 25, .weight = 9, .npc_kind = NK_CIVILIAN, .trig = TRIG_ARRIVAL,
      .title = "DRIFTING TRADER",
      .body = "A FREIGHTER RUNNING DARK FLASHES ITS BAY LIGHTS. '$G, TWO CRATES, 120 FLAT. NO DOCKS, NO DUTIES, NO NAMES.'",
      .texts = e25_tx, .choices = e25_ch, .n_choices = 2 },

    /* continuity */
    { .id = 26, .weight = 12, .flags = EV_ONESHOT, .npc_kind = NK_CIVILIAN,
      .trig = TRIG_BAR, .need_flag = 1 + 1,
      .title = "A FAMILIAR FACE",
      .body = "SOMEONE PUSHES THROUGH THE CROWD TOWARD YOUR TABLE - AND YOU KNOW THE FACE, THOUGH IT LAST LOOKED OUT FROM BEHIND YOUR CARGO RACKS.",
      .texts = e26_tx, .choices = e26_ch, .n_choices = 2 },

    /* faction war */
    { .id = 27, .weight = 13, .npc_kind = NK_OFFICIAL, .trig = TRIG_DOCK,
      .gate = GATE_FRONTLINE | GATE_REP_PLUS,
      .title = "THE RECRUITER",
      .body = "A $F OFFICER WORKS THE ARRIVALS QUEUE, UNIFORM PRESSED, VOICE TIRED. 'THE FRONT NEEDS GUNS. YOURS WILL DO. SIGNING BONUS, COMBAT PAY, GRATITUDE.'",
      .texts = e27_tx, .choices = e27_ch, .n_choices = 2 },

    /* THE POLICY act 2: the Beneficiary (Vessa, fixed_npc 2) */
    { .id = 28, .weight = 13, .npc_kind = NK_CIVILIAN, .trig = TRIG_BAR,
      .need_flag = 1 + 11, .not_flag = 1 + 14, .fixed_npc = 2,
      .title = "THE ARCHIVIST",
      .body = "A WOMAN WITH ARCHIVE-SERVICE INK ON HER KNUCKLES SITS DOWN UNINVITED. 'YOU'RE THE PILOT FROM THE GREY-SHIP BUSINESS. GOOD. I STUDY POLICIES THAT DON'T DIE - AND I FOUND YOUR NAME ON A MANIFEST OLDER THAN YOU.'",
      .texts = e28_tx, .choices = e28_ch, .n_choices = 2 },
    { .id = 29, .weight = 13, .npc_kind = NK_CIVILIAN, .trig = TRIG_BAR,
      .need_flag = 1 + 14, .not_flag = 1 + 15, .fixed_npc = 2,
      .title = "THE LEDGER",
      .body = "VESSA HAS THE CORNER BOOTH AND A SLATE FULL OF YOUR LIFE. 'I TRACED THE PREMIUMS ON THAT UNLAPSED CLAIM,' SHE SAYS, TOO QUIETLY. 'YOU SHOULD SIT.'",
      .texts = e29_tx, .choices = e29_ch, .n_choices = 3 },
    { .id = 30, .weight = 15, .flags = EV_ONESHOT, .npc_kind = NK_CIVILIAN,
      .trig = TRIG_DOCK, .need_flag = 1 + 15, .fixed_npc = 2,
      .title = "THE BENEFICIARY",
      .body = "VESSA IS WAITING AT YOUR PAD, HOLLOW-EYED, HOLDING A SEALED ARCHIVE FLIMSY LIKE IT MIGHT GO OFF. 'THE DISBURSEMENT RECORD. WHO THE PAYOUT WENT TO.' SHE DOESN'T HAND IT OVER SO MUCH AS SURRENDER IT.",
      .texts = e30_tx, .choices = e30_ch, .n_choices = 3 },

    /* THE POLICY act 3: the Terms (the Underwriter returns) */
    { .id = 31, .weight = 60, .flags = EV_ONESHOT, .npc_kind = NK_OFFICIAL,
      .trig = TRIG_ARRIVAL, .need_flag = 1 + 16, .fixed_npc = 1,
      .title = "RECALL NOTICE",
      .body = "THE HAIL ARRIVES BEFORE YOUR DRIVE COOLS, ON A CHANNEL YOUR SHIP SHOULDN'T HAVE. A PLAIN GREY VOICE READS A POLICY NUMBER - YOURS - AND SAYS: 'YOUR CLAIM IS UNDER REVIEW.'",
      .texts = e31_tx, .choices = e31_ch, .n_choices = 2 },
    { .id = 32, .weight = 60, .flags = EV_ONESHOT, .npc_kind = NK_NONE,
      .trig = TRIG_SPACE, .need_flag = 1 + 17,
      .title = "THE AUDIT",
      .body = "THE DERELICT IS GREY, REGISTRYLESS - AND THE LAYOUT IS YOUR SHIP'S, DOWN TO THE WELD YOU NEVER LIKED. ON THE DEAD FLIGHT DECK, ONE RECORDER, STILL WARM.",
      .texts = e32_tx, .choices = e32_ch, .n_choices = 2 },
    { .id = 33, .weight = 15, .flags = EV_ONESHOT, .npc_kind = NK_OFFICIAL,
      .trig = TRIG_DOCK, .need_flag = 1 + 18, .fixed_npc = 1,
      .title = "THE TERMS",
      .body = "THE UNDERWRITER IS IN YOUR BAY WHEN THE CLAMPS ENGAGE, GREY COAT IMMACULATE, A SINGLE PAGE IN HAND. 'AUDIT COMPLETE. REVIEW FAVOURABLE. YOU ARE OWED DISCLOSURE - ONCE. THESE ARE THE TERMS.'",
      .texts = e33_tx, .choices = e33_ch, .n_choices = 3 },

    /* planted-flag payoffs */
    { .id = 34, .weight = 11, .flags = EV_ONESHOT, .npc_kind = NK_MYSTIC,
      .trig = TRIG_BAR, .need_flag = 1 + 2,
      .title = "THE COLLECTOR",
      .body = "A GLOVED STRANGER SETS DOWN A CASE LINED WITH SMALL COLD TOKENS - ELEVEN OF THEM. 'YOU HOLD A RECEIPT OF THE COVER,' THEY SAY. 'THREE HUNDRED CREDITS. MOST DON'T GET AN OFFER.'",
      .texts = e34_tx, .choices = e34_ch, .n_choices = 2 },
    { .id = 35, .weight = 11, .flags = EV_ONESHOT, .npc_kind = NK_NONE,
      .trig = TRIG_DOCK, .need_flag = 1 + 3,
      .title = "RESUBMITTED",
      .body = "THE HARBOURMASTER'S TERMINAL CHIMES TWICE FOR YOU TODAY. THE UNCLAIMED-VESSEL RECORD YOU PURGED IS BACK IN YOUR DOCKET - WITH A CREDIT ATTACHMENT.",
      .texts = e35_tx, .choices = e35_ch, .n_choices = 2 },
    { .id = 36, .weight = 11, .flags = EV_ONESHOT, .npc_kind = NK_DOCKHAND,
      .trig = TRIG_BAR, .need_flag = 1 + 12,
      .title = "THE OTHER YOU",
      .body = "THE BARKEEP DOES A DOUBLE-TAKE THAT LASTS TOO LONG. 'BACK ALREADY? NO - WAIT.' THEY LOWER THE GLASS THEY WERE POLISHING. 'SOMEONE DRANK HERE THREE WEEKS AGO. I'D HAVE SWORN IT WAS YOU.'",
      .texts = e36_tx, .choices = e36_ch, .n_choices = 2 },

    /* standalone texture */
    { .id = 37, .weight = 9, .npc_kind = NK_DOCKHAND, .trig = TRIG_DOCK,
      .title = "UNION DUES",
      .body = "THE LOADING GANG HAS DOWNED TOOLS AND CHAINED THE CRANES. A PICKET CAPTAIN WITH A SPLIT LIP LAYS IT OUT: '$T PAYS HALF-RATES AND BURIES THE INJURED QUIETLY. NOTHING MOVES TODAY. UNLESS YOU MOVE IT.'",
      .texts = e37_tx, .choices = e37_ch, .n_choices = 3 },
    { .id = 38, .weight = 12, .flags = EV_ONESHOT, .npc_kind = NK_NONE,
      .trig = TRIG_DOCK, .need_flag = 1 + 21,
      .title = "THE MEMORIAL",
      .body = "BY THE BAY LIFTS, A WALL OF SMALL BRASS PLATES: EVERY HULL THIS PORT EVER LOST. YOU READ IT THE WAY EVERYONE DOES - UNTIL ONE NAME READS BACK.",
      .texts = e38_tx, .choices = e38_ch, .n_choices = 2 },
    { .id = 39, .weight = 9, .npc_kind = NK_PIRATE, .trig = TRIG_DOCK,
      .gate = GATE_CARGO_SPACE,
      .title = "HOT CARGO",
      .body = "A FORKLIFT 'BREAKS DOWN' BESIDE YOUR PAD. ITS DRIVER TALKS WITHOUT MOVING HIS LIPS: 'UNCLAIMED CRATE, NO QUESTIONS, EIGHTY. GREASE SAYS MACHINE PARTS. GREASE SAYS A LOT OF THINGS.'",
      .texts = e39_tx, .choices = e39_ch, .n_choices = 2 },
    { .id = 40, .weight = 10, .npc_kind = NK_CIVILIAN, .trig = TRIG_BAR,
      .title = "THE CARTOGRAPHER",
      .body = "AN OLD SURVEYOR HAS A CHART SPREAD ACROSS TWO TABLES, ANNOTATED IN FIVE HANDS OVER FIFTY YEARS. 'FRESH LANES, PILOT? I PAY STANDARD RATE FOR HONEST LOGS.'",
      .texts = e40_tx, .choices = e40_ch, .n_choices = 3 },
    { .id = 41, .weight = 8, .flags = EV_ONESHOT, .npc_kind = NK_DOCKHAND,
      .trig = TRIG_BAR,
      .title = "LAST ROUND",
      .body = "THE WHOLE BAR IS DRINKING TO A WHITE-HAIRED PILOT WHOSE DOCKING LICENCE EXPIRED AT MIDNIGHT. HE'S GIVING HIS LIFE AWAY PIECE BY PIECE, AND HE'S JUST SPOTTED YOU.",
      .texts = e41_tx, .choices = e41_ch, .n_choices = 2 },
    { .id = 42, .weight = 10, .npc_kind = NK_NONE, .trig = TRIG_SPACE,
      .title = "GRAVE MARKER",
      .body = "NOT A WRECK - A TOMB. THE HULL IS WELDED SHUT FROM OUTSIDE, RINGED WITH OFFERINGS: COINS, RATION TINS, A CHILD'S TOY SEALED IN RESIN. THE BEACON TICKS LIKE A SLOW HEART.",
      .texts = e42_tx, .choices = e42_ch, .n_choices = 2 },
    { .id = 43, .weight = 9, .npc_kind = NK_NONE, .trig = TRIG_ARRIVAL,
      .gate = GATE_LAWFUL,
      .title = "TOLL GATE",
      .body = "A BARRIER DRONE PARKS ITSELF ACROSS YOUR APPROACH LANE, ALL STRIPES AND CHEERFUL MENACE. 'WELCOME, PILOT! LANE MAINTENANCE LEVY: FIFTY CREDITS. HAVE A COMPLIANT DAY.'",
      .texts = e43_tx, .choices = e43_ch, .n_choices = 2 },
    { .id = 44, .weight = 9, .npc_kind = NK_MYSTIC, .trig = TRIG_ARRIVAL,
      .title = "PILGRIM CONVOY",
      .body = "SIX SLOW BARGES IN PROCESSION, HULLS PAINTED WITH CONSTELLATIONS THAT DON'T EXIST. THE LEAD SHIP HAILS: 'THE LANES AHEAD ARE UNKIND, PILOT. RIDE WITH US A WHILE - THE WATCHED ROAD IS THE SAFE ONE.'",
      .texts = e44_tx, .choices = e44_ch, .n_choices = 2 },

    /* THE POLICY act 4: the ledger bleeds (D) */
    { .id = 45, .weight = 14, .npc_kind = NK_PIRATE, .trig = TRIG_DOCK,
      .need_flag = 1 + 19, .not_flag = 1 + 22, .gate = GATE_FRONTLINE,
      .title = "BOTH SIDES",
      .body = "A SHOT-UP MERCENARY FROM THE OTHER TRENCH IS DYING IN YOUR PAD'S SHADOW, AND WAVES YOU CLOSE LIKE AN OLD FRIEND. 'YOU FLEW THE ZONE TOO. THEN YOU'LL WANT TO SEE WHO PAID ME.'",
      .texts = e45_tx, .choices = e45_ch, .n_choices = 2 },
    { .id = 46, .weight = 50, .flags = EV_ONESHOT, .npc_kind = NK_NONE,
      .trig = TRIG_ARRIVAL, .need_flag = 1 + 22,
      .title = "THE ORIGINATION",
      .body = "YOU DROP EARLY - AND THE SKY IS ALREADY OCCUPIED. A PLAIN GREY SHIP SITS LANCE-ON TO A LOADED FREIGHTER THAT HASN'T NOTICED IT YET. NOTHING ABOUT THIS IS A RESCUE.",
      .texts = e46_tx, .choices = e46_ch, .n_choices = 2 },
    { .id = 47, .weight = 14, .npc_kind = NK_CIVILIAN, .trig = TRIG_BAR,
      .need_flag = 1 + 23, .not_flag = 1 + 24, .fixed_npc = 2,
      .title = "THE MISPRINT",
      .body = "VESSA FINDS YOU THIS TIME, AND SHE'S CARRYING TWO SLATES NOW. 'I KNOW WHAT YOUR DOUBLE IS. SIT DOWN. IT'S WORSE THAN A GHOST.'",
      .texts = e47_tx, .choices = e47_ch, .n_choices = 1 },
    { .id = 48, .weight = 50, .flags = EV_ONESHOT, .npc_kind = NK_NONE,
      .trig = TRIG_SPACE, .need_flag = 1 + 24,
      .title = "THE WRITE-OFF",
      .body = "THE HULK'S HOLD IS EMPTY EXCEPT FOR ONE THING NOBODY STRIPS: A COLONY BEACON, REGISTRY SCRUBBED FROM EVERY CHART YOU OWN - STILL TRANSMITTING TO NOBODY.",
      .texts = e48_tx, .choices = e48_ch, .n_choices = 2 },

    /* act 4b: the Collection (B) */
    { .id = 49, .weight = 50, .flags = EV_ONESHOT, .npc_kind = NK_NONE,
      .trig = TRIG_SPACE, .need_flag = 1 + 25,
      .title = "THE THRESHOLD",
      .body = "THIS WRECK ISN'T DRIFTING - IT'S MOORED. A TOW-LINE OF FUSED GREY CABLE RUNS FROM ITS SPINE TOWARD THE RIM, TAUT, MAINTAINED, AND VERY LONG. YOUR SCOPE FOLLOWS IT OUT PAST THE LAST CHARTED STAR.",
      .texts = e49_tx, .choices = e49_ch, .n_choices = 2 },

    /* act 5: the run (D climax + C door) */
    { .id = 50, .weight = 15, .flags = EV_ONESHOT, .npc_kind = NK_OFFICIAL,
      .trig = TRIG_DOCK, .need_flag = 1 + 31, .fixed_npc = 1,
      .title = "THE TRIAGE",
      .body = "THE UNDERWRITER IS BACK IN YOUR BAY, AND THIS TIME THE PAGE IS A LIST. 'THE BOOK MUST BALANCE. YOU HAVE SEEN WHY. INITIAL TEN LINES - ANY TEN - AND BE GENEROUSLY REMEMBERED. OR DON'T, AND THE LINES CHOOSE THEMSELVES.'",
      .texts = e50_tx, .choices = e50_ch, .n_choices = 2 },
    { .id = 51, .weight = 15, .flags = EV_ONESHOT, .npc_kind = NK_MYSTIC,
      .trig = TRIG_BAR, .need_flag = 1 + 26,
      .title = "THE NETWORK",
      .body = "EVERY LAMP IN THE BAR FLICKERS ONCE AS YOU ENTER - AND THE CORNER TABLE IS WAITING FOR YOU. PEOPLE YOU KNOW. PEOPLE WHO HAVE BEEN STEERING YOU, GENTLY, FOR A LONG TIME.",
      .texts = e51_tx, .choices = e51_ch, .n_choices = 1 },
    { .id = 52, .weight = 15, .flags = EV_ONESHOT, .npc_kind = NK_OFFICIAL,
      .trig = TRIG_DOCK, .need_flag = 1 + 27, .fixed_npc = 1,
      .title = "INDEMNITY RUN",
      .body = "ONE PAGE, TWO LINES, THE UNDERWRITER'S PEN HELD OUT TO YOU. LINE ONE KEEPS YOU COVERED FOREVER. LINE TWO SURRENDERS THE POLICY - SETTLES IT - AND EVERYTHING HELD AGAINST IT GOES FREE. THE PEN IS COLD. IT IS ALWAYS COLD.",
      .texts = e52_tx, .choices = e52_ch, .n_choices = 2 },
    { .id = 53, .weight = 0, .flags = EV_ONESHOT, .npc_kind = NK_OFFICIAL,
      .trig = TRIG_SPACE, .fixed_npc = 1,
      .title = "THE RECALL",
      .body = "A FLAT GREY VOICE FILLS THE COCKPIT. 'POLICYHOLDER. YOUR CONTINUITY IS SCHEDULED FOR RE-ISSUE. HOLD STATION AND POWER DOWN - THE PROCEDURE IS ROUTINE, AND HAS BEEN PERFORMED ON YOU BEFORE. YOU WILL NOT REMEMBER THIS ONE EITHER.' GREY HULLS BLOOM ACROSS THE SCANNER, WEAPONS ALREADY WARMING.",
      .texts = e53_tx, .choices = e53_ch, .n_choices = 1 },
    { .id = 54, .weight = 0, .flags = EV_ONESHOT, .npc_kind = NK_OFFICIAL,
      .trig = TRIG_SPACE, .fixed_npc = 0,
      .title = "CONGRATULATIONS",
      .body = "YOU'VE SEEN THE INDEMNITY'S STORY THROUGH TO ITS END. WHICHEVER WAY THE BOOK BALANCED FOR YOU, YOU MADE IT - WELL FLOWN. THE GALAXY IS STILL YOURS: KEEP TRADING, FIGHTING AND EXPLORING FOR AS LONG AS YOU LIKE.",
      .texts = e54_tx, .choices = e54_ch, .n_choices = 1 },
    { .id = 55, .weight = 10, .npc_kind = NK_DOCKHAND, .trig = TRIG_DOCK,
      .title = "THE TUNER",
      .body = "A TECH IN AN OIL-BLACK APRON NODS AT YOUR PRIMARY GUN. 'I CAN OVERCLOCK THAT. MIGHT MAKE IT SING. MIGHT MAKE IT SULK. 300 EITHER WAY.'",
      .texts = e55_tx, .choices = e55_ch, .n_choices = 2 },
    { .id = 56, .weight = 11, .npc_kind = NK_DOCKHAND, .trig = TRIG_DOCK,
      .title = "THE CRATE",
      .body = "A DECKHAND ROLLS OUT A SEALED REPOSSESSION CRATE - NO MANIFEST, JUST A COLD LOT-SEAL. 'HIGHEST BID OPENS IT. COULD BE GOLD. COULD BE SCRAP.'",
      .texts = e56_tx, .choices = e56_ch, .n_choices = 2 },
    { .id = 57, .weight = 8, .flags = EV_ONESHOT, .npc_kind = NK_DOCKHAND,
      .trig = TRIG_DOCK, .gate = GATE_REP_PLUS,
      .title = "A YARD FAVOUR",
      .body = "A YARD ENGINEER WAVES YOU OVER. 'YOU FLY CLEAN AROUND HERE. LET ME UPRATE SOMETHING FOR YOU - ON THE HOUSE. SHIELD, OR PLATE?'",
      .texts = e57_tx, .choices = e57_ch, .n_choices = 2 },
    { .id = 58, .weight = 11, .npc_kind = NK_NONE, .trig = TRIG_SPACE,
      .title = "FIELD STRIP",
      .body = "THE DERELICT STILL HAS A MODULE BOLTED TO A LIVE HARDPOINT, POWER TRICKLING THROUGH IT. PRY IT LOOSE - OR LEAVE IT, IF THE WIRING SPOOKS YOU.",
      .texts = e58_tx, .choices = e58_ch, .n_choices = 2 },
    { .id = 59, .weight = 10, .npc_kind = NK_OFFICIAL, .trig = TRIG_DOCK,
      .gate = GATE_FRONTLINE,
      .title = "THE QUARTERMASTER",
      .body = "A SURPLUS DEALER WORKS A CRATE-STACK BEHIND THE DROPSHIPS. 'WAR PRICES. CLEAN STANDARD KIT, OR THE HOT STUFF THAT'S STILL SERIAL-STAMPED. YOUR CALL.'",
      .texts = e59_tx, .choices = e59_ch, .n_choices = 3 },
    { .id = 60, .weight = 10, .npc_kind = NK_PIRATE, .trig = TRIG_BAR,
      .title = "THE ARENA",
      .body = "A FIGHT-PIT TOUT BLOCKS YOUR PATH. 'OUR HOUSE ACE IS UNDOCKING NOW. BEAT HER OUT IN THE BLACK AND HER BOUNTY AND HER GUN ARE YOURS. 150 TO TAKE THE LINE.'",
      .texts = e60_tx, .choices = e60_ch, .n_choices = 2 },
};
const int k_n_events = (int)(sizeof k_events / sizeof k_events[0]);

/* --- lore fragments (read again in the station DATABASE) ----------------- */
const Lore k_lore[] = {
    /* 0 */ { "THE COVER CHARGE",
      "A SLICE OF EVERY DOCKING FEE, FUEL CHARGE AND FINE IS QUIETLY SKIMMED OFF AND SENT TO A HIDDEN ACCOUNT NO AUDIT CAN TRACE. THE LEDGERS JUST LABEL IT 'COVER'." },
    /* 1 */ { "THE UNDERWRITERS",
      "PILOTS REPORT PLAIN GREY SHIPS WAITING AT CRASH SITES - AND THE LOGS SHOW THEY ARRIVED HOURS BEFORE THE CRASH HAPPENED. SPACERS CALL THEM UNDERWRITERS." },
    /* 2 */ { "CLAIM DENIED",
      "A COLONY ONCE STOPPED PAYING ITS DUES. NOW EVERY STAR CHART AGREES NO COLONY WAS EVER THERE. IT WASN'T DESTROYED - IT WAS DELETED FROM THE RECORD." },
    /* 3 */ { "THE PROSPECTOR'S CLAIM",
      "A MINER'S CLAIM LISTS COORDINATES OUT BEYOND THE RIM, SIGNED OFF BY 'THE INDEMNITY'. BUT NOTHING IS CHARTED OUT THERE - SO WHAT DID THEY FILE A CLAIM ON?" },
    /* 4 */ { "YOUR FILE",
      "AN OLD INSURANCE RECORD SHOWS YOUR EXACT SHIP, LOST WITH ALL HANDS FORTY YEARS AGO. REGISTERED OWNER: YOU. POLICY STATUS: STILL ACTIVE." },
    /* 5 */ { "DOCTRINE OF THE COVER",
      "NOBODY EVER SIGNS UP FOR THE COVER, AND NOBODY READS THE TERMS. IT TAKES ITS PAYMENT IN FUEL, IN HULL, IN YEARS OF YOUR LIFE. MISS A PAYMENT AND IT NOTICES." },
    /* 6 */ { "THE OTHER POLICYHOLDER",
      "A COLD-SLEEP POD LOGGED YOUR NAME, YOUR FINGERPRINTS AND YOUR BLOOD TYPE - ON A SHIP YOU NEVER FLEW. THE POD WAS STILL WARM. SOMEONE JUST LIKE YOU HAD JUST CLIMBED OUT OF IT." },
    /* 7 */ { "RECALLED",
      "GREY HULLS, NO REGISTRATION, MEMORY BANKS WIPED TOO CLEAN FOR ANY SCRAPPER TO MANAGE. THE UNDERWRITERS DESTROY THEIR OWN SHIPS AND LEAVE NOTHING BEHIND TO SALVAGE." },
    /* 8 */ { "THE UNLAPSED",
      "SOME POLICIES NEVER END. THE ARCHIVE LISTS CLAIMS PAID OUT CENTURIES AGO WHOSE PREMIUMS STILL ARRIVE ON TIME - FROM ACCOUNTS WHOSE OWNERS ARE LONG DEAD. TWELVE ARE KNOWN. ONE IS YOURS." },
    /* 9 */ { "THE PREMIUM",
      "FOLLOW ANY PILOT'S FEES FAR ENOUGH UP THE CHAIN AND A SLICE VANISHES INTO THE COVER. FOLLOW YOURS AND THE SLICE IS BIGGER. YOU AREN'T PAYING FOR INSURANCE - YOU'RE PAYING OFF A DEATH CLAIM. THE DEAD PILOT NAMED ON IT IS YOU." },
    /* 10 */ { "SETTLEMENT",
      "A SEALED PAYOUT RECORD, FORTY YEARS OLD. CLAIM: ONE PILOT, LOST WITH ALL HANDS. STATUS: PAID IN FULL. WHAT WAS PAID OUT: ONE PILOT, BROUGHT BACK AND KEPT FLYING. THE SIGNATURE ON THE RELEASE IS YOURS." },
    /* 11 */ { "UNDER REVIEW",
      "THE GREY SHIPS USED TO TURN UP BEFORE OTHER PEOPLE'S ACCIDENTS. NOW THEY TURN UP WHERE YOU ARE HEADED, WAIT FOR YOU TO ARRIVE ON SCHEDULE, AND FILE A NOTE THAT YOU'RE KEEPING UP YOUR PAYMENTS." },
    /* 12 */ { "THE AUDIT",
      "AN AUDIT LOG, PULLED FROM A GREY WRECK BUILT ON THE BONES OF YOUR OLD SHIP: 'ASSET PERFORMING NORMALLY. STILL VIABLE. NO NEED TO REPLACE IT YET.' THE VOICE ON THE RECORDING IS YOURS." },
    /* 13 */ { "THE TERMS",
      "THE INDEMNITY DOESN'T PROTECT YOU FROM LOSS. IT GUARANTEES NOTHING IS EVER LOST - ONLY REPLACED, THEN BILLED FOR. YOU ARE NOT THE CUSTOMER. YOU ARE WHAT THE POLICY PAID OUT. SO IS EVERY OTHER PILOT STILL FLYING. THE WORST PART ISN'T THAT IT'S TRUE - IT'S THAT IT'S COMPLETELY ROUTINE." },
    /* 14 */ { "THE RECEIPT",
      "THE TOKEN IS MADE OF A METAL NO ONE CAN IDENTIFY, AND IT IS ALWAYS COLD. ELEVEN OTHERS EXIST, AND SOMEONE IS COLLECTING THEM. HOLD IT TO YOUR EAR AND YOU HEAR A SOUND LIKE A LEDGER PAGE TURNING." },
    /* 15 */ { "ARREARS",
      "DELETE THE RECORD AND IT COMES STRAIGHT BACK - WITH A LITTLE EXTRA ADDED ON FOR THE TROUBLE YOU CAUSED. THE SYSTEM DOESN'T MIND BEING DOUBTED. IT DOESN'T MIND ANYTHING AT ALL." },
    /* 16 */ { "CONTINUED",
      "A PILOT WITH YOUR FACE DRANK IN THIS BAR THREE WEEKS BEFORE YOU GOT HERE. TIPPED WELL, PAID IN EXACT CHANGE, WORE GREY. SIGNED THE TAB WITH ONE WORD: 'CONTINUED'." },
    /* 17 */ { "THE WALL",
      "YOUR OLD SHIP'S NAME IS ON THE MEMORIAL TO THE DEAD, FORTY YEARS WEATHERED, AMONG PEOPLE WHO REALLY DIED. SOMEONE HAS RECENTLY SCRATCHED ONE WORD UNDER IT: 'PAID'." },
    /* 18 */ { "BOTH SIDES",
      "EVERY WAR CONTRACT CARRIES THE SAME BROKER'S SEAL - BOTH SIDES, EVERY FRONT. THESE WARS DON'T JUST BREAK OUT. SOMEONE STARTS THEM ON PURPOSE, THE WAY A BANK ISSUES A LOAN." },
    /* 19 */ { "THE ORIGINATION",
      "THE GREY SHIPS DON'T PREDICT DISASTERS - THEY CAUSE THEM. A CLAIM HAS TO EXIST BEFORE IT CAN BE PAID, SO THE INDEMNITY CREATES THE DISASTER, THEN COLLECTS ON IT. FORTY YEARS AGO YOU WEREN'T RESCUED. YOUR DEATH WAS ARRANGED ON PURPOSE - SO THEY COULD 'PAY' THE CLAIM BY MAKING YOU, AND BILL YOU EVER SINCE." },
    /* 20 */ { "DOUBLE ISSUE",
      "YOUR DOUBLE ISN'T A GHOST OR A SPY. IT'S A PRINTING ERROR. THE INDEMNITY IS COPYING PILOTS FASTER THAN IT CAN AFFORD TO, AND NOW THE COPIES ARE COMING OUT DOUBLED. THERE IS NO 'REAL' YOU ANYMORE - JUST COPIES. IT'S A RUN ON THE BANK, AND PILOTS ARE THE MONEY." },
    /* 21 */ { "THE WRITE-OFF",
      "THE ERASED COLONY WASN'T DESTROYED. 'COVERAGE WITHDRAWN' MEANS IT WAS REPOSSESSED - TOWED OFF THE MAP, OUT PAST THE LAST CHARTED STAR. THE INDEMNITY NEVER WASTES ANYTHING IT OWNS." },
    /* 22 */ { "THE COLLECTION",
      "OUT BEYOND THE RIM, RACKED AND LIT: EVERY SHIP, COLONY AND LIFE THE INDEMNITY EVER WROTE OFF OR REPLACED - KEPT IN STORAGE AS COLLATERAL FOR A BOOK THAT NO LONGER ADDS UP. SOMEWHERE IN THOSE ROWS, FORTY YEARS OLDER, IS YOU." },
    /* 23 */ { "THE TRIAGE",
      "THE BOOK ONLY BALANCES ONE WAY NOW: CANCEL COVERAGE, TEN ENTRIES AT A TIME. SOMEONE HAS TO INITIAL THE LIST. IF NOBODY DOES, THE LIST CHOOSES ITS OWN NAMES." },
    /* 24 */ { "THE SURRENDER",
      "IF A POLICY IS GIVEN UP FREELY - BY THE VERY PERSON IT PAID OUT, IN PERSON, KNOWING EXACTLY WHAT THEY ARE - THE CLAIM FINALLY CLOSES. AND A CLOSED CLAIM RELEASES ITS COLLATERAL. TWELVE PILOTS HAVE DONE IT. THE COLD TOKENS ARE THEIR RECEIPTS." },
    /* 25 */ { "KEPT CURRENT",
      "YOU STAY INSURED. THE STORAGE RACK STAYS LOCKED. YOUR PAYOUT SPENDS LIKE REAL MONEY, EVERY FEE KEEPS FEEDING THE COVER, AND SOMEWHERE A LEDGER QUIETLY OPENS A FRESH PAGE WITH YOUR NAME ALREADY AT THE TOP. THE LOOP GOES ON." },
    /* 26 */ { "PAID IN FULL",
      "YOU GIVE UP THE POLICY. THE CLAIM CLOSES FOR GOOD. THE RACK OPENS, AND THE ORIGINAL PILOT - THE REAL ONE - WALKS FREE, OWING NOTHING. SO DO YOU, EXCEPT FOR ONE ORDINARY LIFE, PAID THE WAY EVERYONE PAYS IT. THERE IS EXACTLY ONE OF YOU NOW. FLY CAREFULLY." },
};
const int k_n_lore = (int)(sizeof k_lore / sizeof k_lore[0]);

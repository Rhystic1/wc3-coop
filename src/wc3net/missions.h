#ifndef WC3NET_MISSIONS_H
#define WC3NET_MISSIONS_H

#define MISN_F_NONE    0x00
#define MISN_F_SPECIAL 0x01
#define MISN_F_PLANET  0x02
#define MISN_F_CLOAK   0x04
#define MISN_F_TEMBLOR 0x08
#define MISN_F_SOLO    0x10
#define MISN_F_MULTI   0x20
#define MISN_F_SIM     0x40
#define MISN_F_RREG    0x80

typedef struct wc3_misn_def {
    unsigned char id;
    const char   *name;      // short label shown in Mission Select list
    signed char   def_ship;  // campaign default regs[22] (-1..6)
    signed char   def_w1;    // campaign default regs[23] (0=none, 1..9=pilot)
    signed char   def_w2;    // campaign default regs[58] (for 4-ship missions)
    signed char   def_w3;    // campaign default regs[59] (for 4-ship missions)
    unsigned char team_cnt;  // TEAM entries in mission IFF (1, 2, or 4)
    unsigned char need_cd;   // 1..4 (CD1..CD4 TRE set in WC3.CON)
    unsigned char flags;     // MISN_F_*
    unsigned char rreg1;     // mission-specific PROG reg index (0 = none)
    unsigned char rreg2;     // second mission-specific PROG reg (0 = none)
} wc3_misn_def;

// All 65 valid LOOKMISN entries (IDs 7=misnb004 & 11=misnc004 do not exist on disk).
static const wc3_misn_def g_wc3_missions[] = {
    {  0, "00 Orsini 1 - Patrol (Hellcat)",           1, 6, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    {  1, "01 Orsini 2 - Fighters (Hellcat)",         1, 1, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    {  2, "02 Orsini 3 - Escort Cargo (Arrow)",       0, 6, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    {  3, "03 Orsini 4 - Escort Transp (Arrow) [F]",  0, 6, 0, 0, 2, 1, MISN_F_SPECIAL | MISN_F_RREG,                25,  0 },
    {  4, "04 Tamayo 1 - Strike (T-Bolt)",            2, 8, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    {  5, "05 Tamayo 2 - Hit Convoy (Excal)",         4, 6, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    {  6, "06 Tamayo 3 - Dual Sim (Arrow) [SOLO]",    0, 0, 0, 0, 1, 1, MISN_F_SPECIAL | MISN_F_SOLO,                 0,  0 },
    {  8, "08 Tamayo B1 - Strike (T-Bolt)",           2, 8, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    {  9, "09 Tamayo B2 - Hit Convoy (Excal)",        4, 6, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    { 10, "10 Tamayo B3 - Dual Sim (Arrow) [SOLO/F]", 0, 0, 0, 0, 1, 1, MISN_F_SPECIAL | MISN_F_SOLO | MISN_F_RREG,  26,  0 },
    { 12, "12 Locanda 1 - Scout Intercept (Hellcat)", 1, 6, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    { 13, "13 Locanda 2 - Bioweapon Strakhas (TB)",   2, 6, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    { 14, "14 Locanda 3 - Rescue Flint [SOLO/F]",     1, 0, 0, 0, 1, 1, MISN_F_SPECIAL | MISN_F_SOLO | MISN_F_RREG,  27,  0 },
    { 15, "15 Blackmane 1 - Defend Base (Hellcat)",   1, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 16, "16 Blackmane 2 - Convoy Patrol (T-Bolt)",  2, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 17, "17 Blackmane 3 - Escort Convoy (T-Bolt)",  2, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 18, "18 Blackmane B1 - Escort Jump (Hellcat)",  1, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 19, "19 Blackmane B2 - Convoy Meet (T-Bolt)",   2, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 20, "20 Blackmane B3 - Final Jump (T-Bolt) [F]",2, 6, 0, 0, 2, 2, MISN_F_SPECIAL | MISN_F_RREG,                29, 30 },
    { 21, "21 Ariel 1 - Sweep (Longbow)",             3, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 22, "22 Ariel 2 - Strike (T-Bolt)",             2, 1, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 23, "23 Ariel 3 - Escort Victory (T-Bolt)",     2, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 24, "24 Caliban 1 - Protect DD (Hellcat)",      1, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 25, "25 Caliban 2 - Strike (T-Bolt)",           2, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 26, "26 Caliban 3 - Escort Victory (Hellcat)",  1, 9, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 27, "27 Delius 1 - Asteroid Hunt (Arrow)",      0, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 28, "28 Delius 2 - Cap Ships (Longbow)",        3, 8, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 29, "29 Delius 3 - Asteroid Base (Hellcat)",    1, 6, 0, 0, 2, 2, MISN_F_NONE,                                  0,  0 },
    { 30, "30 Torgo 1 - Wingman Check (Arrow)",       0, 9, 0, 0, 2, 3, MISN_F_NONE,                                  0,  0 },
    { 31, "31 Torgo 2 - Behemoth Cover (Longbow)",    3, 2, 0, 0, 2, 3, MISN_F_NONE,                                  0,  0 },
    { 32, "32 Torgo 3 - Behemoth Run (Hellcat)",      1, 2, 0, 0, 2, 3, MISN_F_NONE,                                  0,  0 },
    { 33, "33 Loki 1 - Behemoth Escort (Hellcat)",    1, 6, 0, 0, 2, 3, MISN_F_NONE,                                  0,  0 },
    { 34, "34 Loki 2 - Behemoth Assault (Hellcat)",   1, 6, 0, 0, 2, 3, MISN_F_NONE,                                  0,  0 },
    { 35, "35 Kilrah 1 - Behemoth Doom (Hellcat)",    1, 6, 0, 0, 2, 3, MISN_F_NONE,                                  0,  0 },
    { 36, "36 Alcor 1 - Patrol (Arrow)",              0, 6, 0, 0, 2, 3, MISN_F_NONE,                                  0,  0 },
    { 37, "37 Alcor 2 - Reconnaissance (Arrow)",      0, 6, 0, 0, 2, 3, MISN_F_NONE,                                  0,  0 },
    { 38, "38 Alcor 3 - Rogue Hobbes (HC) [SOLO]",    1, 0, 0, 0, 1, 3, MISN_F_SPECIAL | MISN_F_SOLO,                 0,  0 },
    { 39, "39 Alcor 4 - Surface Defenses [PLANET]",   4, 0, 0, 0, 1, 3, MISN_F_SPECIAL | MISN_F_PLANET | MISN_F_SOLO, 0,  0 },
    { 40, "40 Freya 1 - Strike (Longbow) [CD4]",      3, 8, 0, 0, 2, 4, MISN_F_NONE,                                  0,  0 },
    { 41, "41 Freya 2 - Atmos Dive [PLANET/SOLO]",    4, 0, 0, 0, 1, 4, MISN_F_SPECIAL | MISN_F_PLANET | MISN_F_SOLO, 0,  0 },
    { 42, "42 Loviatar - Base Def (TB) [CD4]",        2, 6, 0, 0, 2, 4, MISN_F_NONE,                                  0,  0 },
    { 43, "43 Hyperion 1 - Temblor Bomb [PLT/TMBLR]", 6, 0, 0, 0, 1, 4, MISN_F_SPECIAL | MISN_F_PLANET | MISN_F_TEMBLOR | MISN_F_SOLO, 0, 0 },
    { 44, "44 Hyperion 2 - Intercept (HC) [CD4]",     1, 9, 0, 0, 2, 4, MISN_F_NONE,                                  0,  0 },
    { 45, "45 Freya 3 - Search&Destroy (Excal)",      4, 7, 0, 0, 2, 4, MISN_F_NONE,                                  0,  0 },
    { 46, "46 Kilrah 2 - Route (Excal) [3-WING]",     4, 4, 8, 2, 4, 4, MISN_F_SPECIAL | MISN_F_MULTI,                0,  0 },
    { 47, "47 Kilrah 3 - Route (Excal) [3-WING]",     4, 4, 8, 2, 4, 4, MISN_F_SPECIAL | MISN_F_MULTI,                0,  0 },
    { 48, "48 Kilrah 4 - Final Run [CLOAK/PLT/3W]",   5, 2, 8, 9, 4, 4, MISN_F_SPECIAL | MISN_F_CLOAK | MISN_F_PLANET | MISN_F_MULTI | MISN_F_RREG, 43, 0 },
    { 49, "49 Proxima - Jump Defense (Excal) [CD4]",  4, 8, 0, 0, 2, 4, MISN_F_NONE,                                  0,  0 },
    { 50, "50 Sol - Defend Earth (Excal) [CD4]",      4, 7, 0, 0, 2, 4, MISN_F_NONE,                                  0,  0 },
    { 51, "51 Tamayo 2 (Alt NE, T-Bolt)",             2, 6, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    { 52, "52 Tamayo B2 (Alt NE, T-Bolt)",            2, 6, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    { 53, "53 Locanda 3 (Alt BD, Arrow) [SOLO/F]",    0, 0, 0, 0, 1, 1, MISN_F_SPECIAL | MISN_F_SOLO | MISN_F_RREG,  27,  0 },
    { 54, "54 Locanda 1 (Alt B, Hellcat)",            1, 6, 0, 0, 2, 1, MISN_F_NONE,                                  0,  0 },
    { 61, "61 Alcor 1 (Alt D, Arrow)",                0, 6, 0, 0, 2, 3, MISN_F_NONE,                                  0,  0 },
    { 66, "66 Kilrah 0 - Jump Pt (Excal) [3-WING/F]", 4, 4, 8, 2, 4, 4, MISN_F_SPECIAL | MISN_F_MULTI | MISN_F_RREG, 53,  0 },
    // Simulator missions (tsim001..tsim010)
    { 55, "55 Sim 1 - Darket Target (Arrow) [SIM]",   0, 0, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
    { 56, "56 Sim 2 - Darkets + Wing (Arrow) [SIM]",  0, 6, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
    { 57, "57 Sim 3 - Transport Escort (HC) [SIM]",   1, 0, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
    { 58, "58 Sim 4 - Escort + Wing (HC) [SIM]",      1, 6, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
    { 59, "59 Sim 5 - Cap Ship Attack (TB) [SIM]",    2, 0, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
    { 60, "60 Sim 6 - Cap Ship + Wing (TB) [SIM]",    2, 6, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
    { 62, "62 Sim 7 - Longbow Strike [SIM]",          3, 0, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
    { 63, "63 Sim 8 - Longbow + Wing [SIM]",          3, 6, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
    { 64, "64 Sim 9 - Mine Laying (Longbow) [SIM]",   3, 0, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
    { 65, "65 Sim 10 - Endless Survival [SIM]",       4, 0, 0, 0, 1, 1, MISN_F_SIM,                                   0,  0 },
};

#define WC3_MISN_COUNT (int)(sizeof(g_wc3_missions) / sizeof(g_wc3_missions[0]))

static const int g_wc3_pilot_ids[] = { 0, 1, 2, 4, 6, 7, 8, 9 };
static const char * const g_wc3_pilot_names[] = {
    "None (Solo)", "Cobra", "Vagabond", "Flash", "Hobbes", "Vaquero", "Maniac", "Flint"
};
#define WC3_PILOT_COUNT (int)(sizeof(g_wc3_pilot_ids) / sizeof(g_wc3_pilot_ids[0]))

static const char *wc3_ship_name(int id)
{
    switch (id) {
    case -1: return "Mission Default";
    case  0: return "Arrow";
    case  1: return "Hellcat V";
    case  2: return "Thunderbolt VII";
    case  3: return "Longbow";
    case  4: return "Excalibur";
    case  5: return "Excalibur (Cloak)";
    case  6: return "Excalibur (Temblor)";
    default: return "Unknown";
    }
}

static const char *wc3_pilot_name(int id)
{
    int i;
    for (i = 0; i < WC3_PILOT_COUNT; i++) {
        if (g_wc3_pilot_ids[i] == id) return g_wc3_pilot_names[i];
    }
    return "None (Solo)";
}

static void wc3_rreg_info(int reg, const char **out_label, const char **out_desc)
{
    switch (reg) {
    case 25:
        *out_label = "Reg 25: Orsini Cargo";
        *out_desc  = "0=Cargo lost in Orsini 3, 1=Cargo survived";
        break;
    case 26:
        *out_label = "Reg 26: Tamayo Run";
        *out_desc  = "0=Failed/Skipped Tamayo 1, 1=Tamayo 1 success";
        break;
    case 27:
        *out_label = "Reg 27: Locanda Bio";
        *out_desc  = "0=Bioweapon survived, 1=All bioweapons destroyed";
        break;
    case 29:
        *out_label = "Reg 29: Blackmane Convoy 1";
        *out_desc  = "0=Convoy 1 lost, 1=Convoy 1 reached jump";
        break;
    case 30:
        *out_label = "Reg 30: Blackmane Convoy 2";
        *out_desc  = "0=Convoy 2 lost, 1=Convoy 2 survived";
        break;
    case 43:
        *out_label = "Reg 43: Alcor / Hobbes Flag";
        *out_desc  = "Kilrah Final Run campaign state flag (0 or 1)";
        break;
    case 53:
        *out_label = "Reg 53: Alcor Stealth Flag";
        *out_desc  = "Kilrah Jump Point prologue flag (0 or 1)";
        break;
    default:
        *out_label = "Mission Flag";
        *out_desc  = "Mission PROG state register (0 or 1)";
        break;
    }
}

static int wc3_mission_need_cd(int mission_id)
{
    int i;
    for (i = 0; i < WC3_MISN_COUNT; i++) {
        if ((int)g_wc3_missions[i].id == mission_id)
            return (int)g_wc3_missions[i].need_cd;
    }
    return 1;
}

#endif

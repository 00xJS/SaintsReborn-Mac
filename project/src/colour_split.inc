// 25. COLOUR PASS SPLIT (2026-09-30, build cs1). The game records the main colour pass as ONE job (821915E8 on a
//    worker: begin a command buffer 8218DA88, draw the whole main render list 82191328, end 8218DC50); the render
//    thread joins it in 821929C8 (~1.5 ms wait per frame = the game is render-thread bound, see handoff) and inserts
//    its command buffer (82196BF8). Split: two jobs (job arg r5 = 1 / 2 via descriptor +24), each drawing half of
//    the list into its own command buffer (half 2 takes pool C, type 0, which the game never uses); the render
//    thread inserts A then B, so the GPU sees the same order. Only when <= 2 shadow views this frame (pools B+C
//    share 3 command memories per frame) and >= 64 entries.
//    dist\colour_split = on; dist\colour_split_ab = on/off every 10 s ("COLOUR SPLIT" log: fps per mode);
//    colour_split_ratio.txt = share of entries in the first half (default 0.5).
namespace colour_split {
thread_local int tl_begin = 0, tl_end = 0x7FFFFFFF;
thread_local bool tl_force_pool_c = false;
inline int64_t FirstCount(uint32_t count) { return int64_t(std::min<int32_t>(int32_t(count), tl_end)) - tl_begin; }
inline int64_t LoopCount(uint32_t count) { return std::min<int32_t>(int32_t(count), tl_end); }
bool On() { static const bool on = FileExists("colour_split"); return on; }
bool AbOn() { static const bool on = FileExists("colour_split_ab"); return on; }
double Ratio() {
    static const double r = [] {
        double v = 0.5;
        if (FILE* f = std::fopen("colour_split_ratio.txt", "rb")) { if (std::fscanf(f, "%lf", &v) != 1) v = 0.5; std::fclose(f); }
        return std::clamp(v, 0.1, 0.9);
    }();
    return r;
}
std::atomic<bool> ab_active{true};
std::atomic<int32_t> mid{0};
std::atomic<uint32_t> pending_b{0};   // completion handle of job B (0 = none)
std::atomic<uint32_t> count_b{0};
uint32_t slot_b = 0;                  // guest word: job B's command buffer handle
// Pool C (type 0) buffers get NO command memory from the game (82196A38's type-0 path never sets +5436; cs1
// froze on that). Ours, as 82196500 builds the game's (4 MB like the colour pass's), one per frame parity.
uint32_t cmd_mem[2] = {0, 0};
bool cmd_mem_failed = false;
std::atomic<int> trace{0};
std::atomic<bool> a_done{false};  // colour_split_serial: half B records only after half A finished
bool Serial() { static const bool on = FileExists("colour_split_serial"); return on; }  // first splits: every step logged
#define CS_TRACE(...) do { if (colour_split::trace.load() < 5) REXLOG_INFO("COLOUR SPLIT TRACE: " __VA_ARGS__); } while (0)
// Context index (buffer +5432) -> device, through the game's tables (8263DCF0 -> 82631000 -> 826460D8).
uint32_t DeviceOf(uint8_t* base, uint32_t idx) {
    if (idx > 64) return 0;
    const uint32_t j = Rd32(base, 0x840B5040u + idx * 192 + 176);
    if (j > 64) return 0;
    const uint32_t k = Rd32(base, 0x84097210u + j * 5680 + 5664);
    if (k > 64) return 0;
    return Rd32(base, 0x840B56B0u + k * 160);
}
int devices_ok = -1;  // -1 unknown, 0 shared (never split), 1 distinct
bool CheckDevices(uint8_t* base) {
    if (devices_ok >= 0) return devices_ok == 1;
    const uint32_t objs[6] = {0x838EB3E0u, 0x838EC950u, 0x838EC950u + 5456, 0x838EC950u + 2 * 5456, 0x838F0960u, 0x838F0960u + 5456};
    const char* names[6] = {"A0 (colour)", "B0", "B1", "B2", "C0", "C1"};
    uint32_t dev[6];
    std::string s = fmt::format("render thread default device {:08X}", Rd32(base, 0x837103A0u));
    for (int i = 0; i < 6; ++i) {
        const uint32_t idx = Rd32(base, objs[i] + 5432);
        dev[i] = DeviceOf(base, idx);
        s += fmt::format(" | {} handle {:08X} ctx {} device {:08X}", names[i], Rd32(base, objs[i] + 5428), idx, dev[i]);
    }
    const bool ok = dev[4] && dev[5] && dev[4] != dev[0] && dev[5] != dev[0] && dev[4] != dev[5] &&
                    dev[4] != Rd32(base, 0x837103A0u) && dev[5] != Rd32(base, 0x837103A0u);
    devices_ok = ok ? 1 : 0;
    REXLOG_INFO("COLOUR SPLIT devices: {} => {}", s, ok ? "pool C has its own devices, split allowed" : "SHARED - split disabled");
    return ok;
}
std::atomic<uint64_t> splits{0}, skipped_shadows{0}, skipped_small{0}, skipped_pending{0}, no_memory{0},
    join_b_ticks{0}, join_b_count{0};
}  // namespace colour_split

static void sr_colour_list(PPCContext& __restrict ctx, uint8_t* base) {  // copy of 82191328, loop range patched
	REX_FUNC_PROLOGUE();
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// bl 0x8270286c
	ctx.lr = 0x82191330;
	__savegprlr_21(ctx, base);
	// stwu r1,-352(r1)
	ea = -352 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lwz r29,0(r13)
	ctx.r29.u64 = REX_LOAD_U32(ctx.r13.u32 + 0);
	// li r11,7696
	ctx.r11.s64 = 7696;
	// li r27,7692
	ctx.r27.s64 = 7692;
	// li r31,0
	ctx.r31.s64 = 0;
	// li r8,-1
	ctx.r8.s64 = -1;
	// mr r30,r3
	ctx.r30.u64 = ctx.r3.u64;
	// lwzx r9,r29,r11
	ctx.r9.u64 = REX_LOAD_U32(ctx.r29.u32 + ctx.r11.u32);
	// li r10,384
	ctx.r10.s64 = 384;
	// lwzx r11,r29,r27
	ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + ctx.r27.u32);
	// stw r31,108(r9)
	REX_STORE_U32(ctx.r9.u32 + 108, ctx.r31.u32);
	// stw r31,112(r9)
	REX_STORE_U32(ctx.r9.u32 + 112, ctx.r31.u32);
	// addi r9,r11,4
	ctx.r9.s64 = ctx.r11.s64 + 4;
	// stw r8,4252(r11)
	REX_STORE_U32(ctx.r11.u32 + 4252, ctx.r8.u32);
	// stw r8,4256(r11)
	REX_STORE_U32(ctx.r11.u32 + 4256, ctx.r8.u32);
	// stw r8,4260(r11)
	REX_STORE_U32(ctx.r11.u32 + 4260, ctx.r8.u32);
loc_82191370:
	// addi r10,r10,-1
	ctx.r10.s64 = ctx.r10.s64 + -1;
	// stb r31,0(r9)
	REX_STORE_U8(ctx.r9.u32 + 0, ctx.r31.u8);
	// addi r9,r9,8
	ctx.r9.s64 = ctx.r9.s64 + 8;
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// bne cr6,0x82191370
	if (!ctx.cr6.eq) goto loc_82191370;
	// bl 0x8219e230
	ctx.lr = 0x82191388;
	sub_8219E230(ctx, base);
	// bl 0x8219e548
	ctx.lr = 0x8219138C;
	sub_8219E548(ctx, base);
	// lwzx r11,r29,r27
	ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + ctx.r27.u32);
	// li r10,8
	ctx.r10.s64 = 8;
	// stw r10,3932(r11)
	REX_STORE_U32(ctx.r11.u32 + 3932, ctx.r10.u32);
	// bl 0x8263ecd0
	ctx.lr = 0x8219139C;
	sub_8263ECD0(ctx, base);
	// lwzx r11,r29,r27
	ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + ctx.r27.u32);
	// clrlwi r10,r30,24
	ctx.r10.u64 = ctx.r30.u32 & 0xFF;
	// addi r3,r1,96
	ctx.r3.s64 = ctx.r1.s64 + 96;
	// stw r10,4284(r11)
	REX_STORE_U32(ctx.r11.u32 + 4284, ctx.r10.u32);
	// bl 0x8212bc88
	ctx.lr = 0x821913B0;
	sub_8212BC88(ctx, base);
	// lis r11,-31885
	ctx.r11.s64 = -2089615360;
	// addi r3,r1,176
	ctx.r3.s64 = ctx.r1.s64 + 176;
	// addi r4,r11,29616
	ctx.r4.s64 = ctx.r11.s64 + 29616;
	// li r5,68
	ctx.r5.s64 = 68;
	// bl 0x82702900
	ctx.lr = 0x821913C4;
	sub_82702900(ctx, base);
	// addi r4,r1,176
	ctx.r4.s64 = ctx.r1.s64 + 176;
	// addi r3,r1,96
	ctx.r3.s64 = ctx.r1.s64 + 96;
	// bl 0x8223a298
	ctx.lr = 0x821913D0;
	sub_8223A298(ctx, base);
	// lis r11,-32131
	ctx.r11.s64 = -2105737216;
	// stb r31,80(r1)
	REX_STORE_U8(ctx.r1.u32 + 80, ctx.r31.u8);
	// stb r31,81(r1)
	REX_STORE_U8(ctx.r1.u32 + 81, ctx.r31.u8);
	// lbz r11,27799(r11)
	ctx.r11.u64 = REX_LOAD_U8(ctx.r11.u32 + 27799);
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x821913f0
	if (ctx.cr6.eq) goto loc_821913F0;
	// li r11,1
	ctx.r11.s64 = 1;
	// stb r11,81(r1)
	REX_STORE_U8(ctx.r1.u32 + 81, ctx.r11.u8);
loc_821913F0:
	// lis r11,-31887
	ctx.r11.s64 = -2089746432;
	// addi r25,r11,-10636
	ctx.r25.s64 = ctx.r11.s64 + -10636;
	// lfs f1,200(r25)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r25.u32 + 200);
	ctx.f1.f64 = double(temp.f32);
	// bl 0x82631bf0
	ctx.lr = 0x82191400;
	sub_82631BF0(ctx, base);
	// lfs f1,204(r25)
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_LOAD_U32(ctx.r25.u32 + 204);
	ctx.f1.f64 = double(temp.f32);
	// bl 0x82631dc8
	ctx.lr = 0x82191408;
	sub_82631DC8(ctx, base);
	// lis r23,-31887
	ctx.r23.s64 = -2089746432;
	// mr r21,r31
	ctx.r21.u64 = ctx.r31.u64;
	// lwz r11,-10392(r23)
	ctx.r11.s64 = colour_split::FirstCount(REX_LOAD_U32(ctx.r23.u32 + -10392));  // PATCH: entries in this job's range
	// cmpwi cr6,r11,0
	ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
	// ble cr6,0x8219158c
	if (!ctx.cr6.gt) goto loc_8219158C;
	// lis r11,-31860
	ctx.r11.s64 = -2087976960;
	// li r24,3
	ctx.r24.s64 = 3;
	// addi r11,r11,-312
	ctx.r11.s64 = ctx.r11.s64 + -312;
	// li r22,160
	ctx.r22.s64 = 160;
	// addi r28,r11,8
	ctx.r28.s64 = ctx.r11.s64 + 8;
	// lis r11,-32131
	ctx.r11.s64 = -2105737216;
	// addi r26,r11,29364
	ctx.r26.s64 = ctx.r11.s64 + 29364;
	ctx.r31.s64 = colour_split::tl_begin;  // PATCH: start of this job's range
	ctx.r28.s64 = ctx.r28.s64 + int64_t(colour_split::tl_begin) * 36;
loc_82191438:
	// lbz r11,4(r28)
	ctx.r11.u64 = REX_LOAD_U8(ctx.r28.u32 + 4);
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x82191578
	if (ctx.cr6.eq) goto loc_82191578;
	// lwz r11,0(r28)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r28.u32 + 0);
	// rlwinm r10,r11,0,9,9
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x400000;
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// bne cr6,0x82191578
	if (!ctx.cr6.eq) goto loc_82191578;
	// rlwinm r10,r11,0,25,25
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x40;
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// bne cr6,0x82191578
	if (!ctx.cr6.eq) goto loc_82191578;
	// lbz r10,16(r28)
	ctx.r10.u64 = REX_LOAD_U8(ctx.r28.u32 + 16);
	// rlwinm r10,r10,0,26,26
	ctx.r10.u64 = __builtin_rotateleft64(ctx.r10.u32 | (ctx.r10.u64 << 32), 0) & 0x20;
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// bne cr6,0x82191578
	if (!ctx.cr6.eq) goto loc_82191578;
	// lbz r10,-1585(r26)
	ctx.r10.u64 = REX_LOAD_U8(ctx.r26.u32 + -1585);
	// rlwinm r30,r11,21,30,31
	ctx.r30.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 21) & 0x3;
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// bne cr6,0x82191488
	if (!ctx.cr6.eq) goto loc_82191488;
	// cmpwi cr6,r30,0
	ctx.cr6.compare<int32_t>(ctx.r30.s32, 0, ctx.xer);
	// bne cr6,0x82191578
	if (!ctx.cr6.eq) goto loc_82191578;
loc_82191488:
	// rlwinm r11,r11,0,24,24
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x80;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x821914e8
	if (ctx.cr6.eq) goto loc_821914E8;
	// lwz r11,0(r26)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r26.u32 + 0);
	// li r3,100
	ctx.r3.s64 = 100;
	// mulli r10,r11,156
	ctx.r10.s64 = static_cast<int64_t>(ctx.r11.u64 * static_cast<uint64_t>(156));
	// lwz r11,0(r25)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r25.u32 + 0);
	// lwzx r11,r10,r11
	ctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + ctx.r11.u32);
	// cntlzw r11,r11
	ctx.r11.u64 = ctx.r11.u32 == 0 ? 32 : __builtin_clz(ctx.r11.u32);
	// rlwinm r11,r11,27,31,31
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 27) & 0x1;
	// xori r11,r11,1
	ctx.r11.u64 = ctx.r11.u64 ^ 1;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// lwzx r11,r29,r27
	ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + ctx.r27.u32);
	// beq cr6,0x821914dc
	if (ctx.cr6.eq) goto loc_821914DC;
	// li r4,3
	ctx.r4.s64 = 3;
	// stw r24,3924(r11)
	REX_STORE_U32(ctx.r11.u32 + 3924, ctx.r24.u32);
	// bl 0x8219f258
	ctx.lr = 0x821914CC;
	sub_8219F258(ctx, base);
	// li r4,1
	ctx.r4.s64 = 1;
	// li r3,1
	ctx.r3.s64 = 1;
	// bl 0x8219f2b0
	ctx.lr = 0x821914D8;
	sub_8219F2B0(ctx, base);
	// b 0x821914e8
	goto loc_821914E8;
loc_821914DC:
	// li r4,160
	ctx.r4.s64 = 160;
	// stw r22,3924(r11)
	REX_STORE_U32(ctx.r11.u32 + 3924, ctx.r22.u32);
	// bl 0x8219f258
	ctx.lr = 0x821914E8;
	sub_8219F258(ctx, base);
loc_821914E8:
	// mr r4,r30
	ctx.r4.u64 = ctx.r30.u64;
	// addi r3,r1,80
	ctx.r3.s64 = ctx.r1.s64 + 80;
	// bl 0x82189658
	ctx.lr = 0x821914F4;
	sub_82189658(ctx, base);
	// lwz r3,-8(r28)
	ctx.r3.u64 = REX_LOAD_U32(ctx.r28.u32 + -8);
	// lwz r6,24(r28)
	ctx.r6.u64 = REX_LOAD_U32(ctx.r28.u32 + 24);
	// addi r5,r1,96
	ctx.r5.s64 = ctx.r1.s64 + 96;
	// li r4,8
	ctx.r4.s64 = 8;
	// lwz r11,0(r3)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);
	// lwz r11,44(r11)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 44);
	// mtctr r11
	ctx.ctr.u64 = ctx.r11.u64;
	// bctrl 
	ctx.lr = 0x82191514;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// clrlwi r11,r3,24
	ctx.r11.u64 = ctx.r3.u32 & 0xFF;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x82191524
	if (ctx.cr6.eq) goto loc_82191524;
	// addi r21,r21,1
	ctx.r21.s64 = ctx.r21.s64 + 1;
loc_82191524:
	// lwz r11,0(r28)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r28.u32 + 0);
	// rlwinm r11,r11,0,24,24
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 0) & 0x80;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x82191578
	if (ctx.cr6.eq) goto loc_82191578;
	// lwz r11,0(r26)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r26.u32 + 0);
	// mulli r10,r11,156
	ctx.r10.s64 = static_cast<int64_t>(ctx.r11.u64 * static_cast<uint64_t>(156));
	// lwz r11,0(r25)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r25.u32 + 0);
	// lwzx r11,r10,r11
	ctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + ctx.r11.u32);
	// cntlzw r11,r11
	ctx.r11.u64 = ctx.r11.u32 == 0 ? 32 : __builtin_clz(ctx.r11.u32);
	// rlwinm r11,r11,27,31,31
	ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 27) & 0x1;
	// xori r11,r11,1
	ctx.r11.u64 = ctx.r11.u64 ^ 1;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x82191564
	if (ctx.cr6.eq) goto loc_82191564;
	// li r4,1
	ctx.r4.s64 = 1;
	// li r3,0
	ctx.r3.s64 = 0;
	// bl 0x8219f2b0
	ctx.lr = 0x82191564;
	sub_8219F2B0(ctx, base);
loc_82191564:
	// lwzx r11,r29,r27
	ctx.r11.u64 = REX_LOAD_U32(ctx.r29.u32 + ctx.r27.u32);
	// li r4,3
	ctx.r4.s64 = 3;
	// li r3,100
	ctx.r3.s64 = 100;
	// stw r24,3924(r11)
	REX_STORE_U32(ctx.r11.u32 + 3924, ctx.r24.u32);
	// bl 0x8219f258
	ctx.lr = 0x82191578;
	sub_8219F258(ctx, base);
loc_82191578:
	// lwz r11,-10392(r23)
	ctx.r11.s64 = colour_split::LoopCount(REX_LOAD_U32(ctx.r23.u32 + -10392));  // PATCH: end of this job's range
	// addi r31,r31,1
	ctx.r31.s64 = ctx.r31.s64 + 1;
	// addi r28,r28,36
	ctx.r28.s64 = ctx.r28.s64 + 36;
	// cmpw cr6,r31,r11
	ctx.cr6.compare<int32_t>(ctx.r31.s32, ctx.r11.s32, ctx.xer);
	// blt cr6,0x82191438
	if (ctx.cr6.lt) goto loc_82191438;
loc_8219158C:
	// lbz r11,80(r1)
	ctx.r11.u64 = REX_LOAD_U8(ctx.r1.u32 + 80);
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x821915a0
	if (ctx.cr6.eq) goto loc_821915A0;
	// bl 0x82631cf0
	ctx.lr = 0x8219159C;
	sub_82631CF0(ctx, base);
	// bl 0x82631ec8
	ctx.lr = 0x821915A0;
	sub_82631EC8(ctx, base);
loc_821915A0:
	// lbz r31,81(r1)
	ctx.r31.u64 = REX_LOAD_U8(ctx.r1.u32 + 81);
	// cmplwi cr6,r31,0
	ctx.cr6.compare<uint32_t>(ctx.r31.u32, 0, ctx.xer);
	// beq cr6,0x821915b8
	if (ctx.cr6.eq) goto loc_821915B8;
	// li r4,1
	ctx.r4.s64 = 1;
	// li r3,48
	ctx.r3.s64 = 48;
	// bl 0x8219f258
	ctx.lr = 0x821915B8;
	sub_8219F258(ctx, base);
loc_821915B8:
	// bl 0x82631cf0
	ctx.lr = 0x821915BC;
	sub_82631CF0(ctx, base);
	// bl 0x82631ec8
	ctx.lr = 0x821915C0;
	sub_82631EC8(ctx, base);
	// bl 0x8219ef10
	ctx.lr = 0x821915C4;
	sub_8219EF10(ctx, base);
	// cmplwi cr6,r31,0
	ctx.cr6.compare<uint32_t>(ctx.r31.u32, 0, ctx.xer);
	// beq cr6,0x821915d8
	if (ctx.cr6.eq) goto loc_821915D8;
	// li r4,1
	ctx.r4.s64 = 1;
	// li r3,48
	ctx.r3.s64 = 48;
	// bl 0x8219f258
	ctx.lr = 0x821915D8;
	sub_8219F258(ctx, base);
loc_821915D8:
	// mr r3,r21
	ctx.r3.u64 = ctx.r21.u64;
	// addi r1,r1,352
	ctx.r1.s64 = ctx.r1.s64 + 352;
	// b 0x827028bc
	__restgprlr_21(ctx, base);
	return;
}


extern "C" void __imp__sub_826374B0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82196BF8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82196A38(PPCContext& ctx, uint8_t* base);
namespace colour_split {
inline bool Active() { return On() && (!AbOn() || ab_active.load(std::memory_order_relaxed)); }
struct CtxGuard {  // keep the caller's registers around extra guest calls
    PPCContext& ctx;
    PPCContext saved;
    explicit CtxGuard(PPCContext& c) : ctx(c), saved(c) {}
    ~CtxGuard() { const auto fp = ctx.fpscr; ctx = saved; ctx.fpscr = fp; }
};
bool EnsureCmdMem(PPCContext& ctx, uint8_t* base) {
    if (cmd_mem[0] && cmd_mem[1]) return true;
    if (cmd_mem_failed) return false;
    CtxGuard g(ctx);
    for (int i = 0; i < 2; ++i) {
        if (cmd_mem[i]) continue;
        ctx.r3.u64 = 184; ctx.r4.u64 = 0x64800000; ctx.lr = 0x82196578;
        sub_82717A98(ctx, base);
        const uint32_t desc = ctx.r3.u32;
        if (!desc) { cmd_mem_failed = true; break; }
        ctx.r3.u64 = 0x400000; ctx.r4.u64 = 0x85800000; ctx.lr = 0x821965A4;
        sub_82717A98(ctx, base);
        const uint32_t mem = ctx.r3.u32;
        if (!mem) { cmd_mem_failed = true; break; }
        Wr32(base, desc + 0, 0x00100009); Wr32(base, desc + 4, 1); Wr32(base, desc + 20, 0xFFFF0000);
        Wr32(base, desc + 152, mem); Wr32(base, desc + 156, 0x400000);
        cmd_mem[i] = desc;
    }
    REXLOG_INFO("COLOUR SPLIT: command memory for half B {} ({:08X} {:08X})", cmd_mem_failed ? "FAILED" : "created",
                cmd_mem[0], cmd_mem[1]);
    return !cmd_mem_failed;
}
// Render thread, 82191EE8 submitting the colour job. true = handled (skip the original call).
bool Submit(PPCContext& ctx, uint8_t* base) {
    if (!Active()) return false;
    const int32_t count = int32_t(Rd32(base, 0x8370D768));
    const int32_t shadows = int32_t(Rd32(base, 0x8370D0D4));
    if (pending_b.load()) { ++skipped_pending; return false; }
    if (count < 64) { ++skipped_small; return false; }
    if (shadows > 2 || shadows < 0) { ++skipped_shadows; return false; }
    if (!CheckDevices(base)) return false;
    if (!EnsureCmdMem(ctx, base)) return false;
    if (!slot_b) {
        slot_b = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(16);
        if (!slot_b) return false;
        Wr32(base, slot_b, 0);
    }
    // Pool C's devices never recorded anything, so they lack the persistent shader constants pool A's device keeps
    // from frame to frame (cs6 diff: zero matrices -> half B invisible). Copy A's float constants (VS + PS).
    {
        const uint32_t dev_a = DeviceOf(base, Rd32(base, 0x838EB3E0u + 5432));
        for (int c = 0; c < 2; ++c) {
            const uint32_t dev_c = DeviceOf(base, Rd32(base, 0x838F0960u + c * 5456u + 5432));
            if (dev_a && dev_c && dev_a != dev_c) std::memcpy(base + dev_c + 1792, base + dev_a + 1792, 9984 - 1792);
            // cs8: also the render struct's constant-set storage (+4400..+5040: camera position for fog etc.; cs7
            // showed half B fogged out). The pointer table at +4292 is per struct and is not copied.
            // cs10: the whole render struct (state caches, sampler-state cache: A has -1 = unknown, C had 0 =
            // "already 0" -> real state changes skipped; serial test proved it is state, not concurrency), except
            // the per-struct pointer table (+4292..+4400) and the command buffer fields (+5428..).
            std::memcpy(base + 0x838F0960u + c * 5456u, base + 0x838EB3E0u, 4292);
            std::memcpy(base + 0x838F0960u + c * 5456u + 4400, base + 0x838EB3E0u + 4400, 5428 - 4400);
        }
    }
    mid.store(std::clamp<int32_t>(int32_t(count * Ratio()), 1, count - 1));
    a_done.store(false);
    const uint32_t desc = ctx.r4.u32;
    CS_TRACE("submit: {} entries, split at {}, shadows {}, handle A {:08X}", count, mid.load(), shadows, ctx.r5.u32);
    Wr32(base, desc + 24, 1);           // job A: first half
    __imp__sub_826374B0(ctx, base);
    {
        CtxGuard g(ctx);
        ctx.lr = 0x82191F78;
        sub_82637238(ctx, base);         // a second completion handle
        const uint32_t hb = ctx.r3.u32;
        Wr32(base, desc + 24, 2);       // job B: second half
        // NOT 826374B0 again: that is a one-job mailbox for the dedicated colour worker (cs3: job B overwrote A,
        // A never ran, the join hung). B goes through the general job queue (826373A8 with r3 = 0), as the
        // render-list prep jobs do.
        ctx.r3.u64 = 0; ctx.r4.u64 = desc; ctx.r5.u64 = hb; ctx.lr = 0x82190614;
        sub_826373A8(ctx, base);
        Wr32(base, desc + 24, 0);
        pending_b.store(hb);
        CS_TRACE("submitted A and B (handle B {:08X})", hb);
    }
    ++splits;
    return true;
}
// Worker: the colour job 821915E8(job, r4 flag, r5 part). true = handled.
bool Half(PPCContext& ctx, uint8_t* base) {
    const uint32_t part = ctx.r5.u32;
    if (part != 1 && part != 2) return false;
    const uint32_t flag = ctx.r4.u32;
    const uint32_t slot = part == 1 ? 0x829E1370u : slot_b;
    CtxGuard g(ctx);
    if (part == 2 && Serial()) {
        const auto t0 = std::chrono::steady_clock::now();
        while (!a_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200))
            _mm_pause();
    }
    tl_force_pool_c = part == 2;
    ctx.r3.u64 = slot; ctx.r4.u64 = 1; ctx.lr = 0x82191614;
    CS_TRACE("job {} begin (thread {:X})", part, GetCurrentThreadId());
    sub_8218DA88(ctx, base);             // begin the command buffer
    tl_force_pool_c = false;
    ctx.r3.u64 = Rd32(base, slot); ctx.lr = 0x82191634;
    sub_821968F8(ctx, base);
    const uint32_t cmd = ctx.r3.u32;
    {
        const uint32_t tls = Rd32(base, g.saved.r13.u32);
        CS_TRACE("job {} began: handle {:08X} buffer {:08X} memory {:08X} ctx {} | tls device {:08X} render {:08X}", part,
                 Rd32(base, slot), cmd, cmd ? Rd32(base, cmd + 5436) : 0, cmd ? Rd32(base, cmd + 5432) : 0,
                 Rd32(base, tls + 7688), Rd32(base, tls + 7692));
    }
    if (part == 2 && cmd && Rd32(base, cmd + 5436) == 0) ++no_memory;  // (would have failed in the game too)
    if (trace.load() == 0) {  // cs6 diagnostic: render struct + recording device right after begin, both halves
        static std::mutex m;
        static std::vector<uint8_t> snap_s[2], snap_d[2];
        static bool logged = false;
        const uint32_t tls = Rd32(base, g.saved.r13.u32);
        const uint32_t dev = Rd32(base, tls + 7688);
        std::lock_guard l(m);
        auto& ss = snap_s[part - 1]; auto& sd = snap_d[part - 1];
        ss.assign(base + cmd, base + cmd + 5456);
        sd.assign(base + dev + 9984, base + dev + 13312);  // cs9: non-constant device state only
        {
            static std::vector<uint8_t> snap_x[2];
            const uint32_t desc = cmd ? Rd32(base, cmd + 5436) : 0;
            if (desc) snap_x[part - 1].assign(base + desc, base + desc + 184);
            static bool xl = false;
            if (!xl && !snap_x[0].empty() && !snap_x[1].empty()) {
                xl = true;
                std::string d;
                for (size_t o = 0; o < 184; o += 4) {
                    uint32_t x, y; std::memcpy(&x, &snap_x[0][o], 4); std::memcpy(&y, &snap_x[1][o], 4);
                    d += fmt::format(" +{}:{:08X}/{:08X}", o, __builtin_bswap32(x), __builtin_bswap32(y));
                }
                REXLOG_INFO("COLOUR SPLIT command buffer descriptors A/C:{}", d);
            }
        }
        if (!logged && !snap_s[0].empty() && !snap_s[1].empty()) {
            logged = true;
            for (int w = 0; w < 2; ++w) {
                const auto& a = w ? snap_d[0] : snap_s[0];
                const auto& c = w ? snap_d[1] : snap_s[1];
                std::string d; int n = 0;
                for (size_t o = 0; o + 4 <= a.size(); o += 4) {
                    uint32_t x, y; std::memcpy(&x, &a[o], 4); std::memcpy(&y, &c[o], 4);
                    if (x != y) { if (n < 150) d += fmt::format(" +{}:{:08X}/{:08X}", o, __builtin_bswap32(x), __builtin_bswap32(y)); ++n; }
                }
                REXLOG_INFO("COLOUR SPLIT DIFF after begin, {} A/C ({} words differ):{}", w ? "DEVICE (offsets from +9984)" : "render struct", n, d);
            }
        }
    }
    tl_begin = part == 1 ? 0 : mid.load();
    tl_end = part == 1 ? mid.load() : 0x7FFFFFFF;
    ctx.r3.u64 = flag ? 1 : 0; ctx.lr = 0x82191624;
    sr_colour_list(ctx, base);
    tl_begin = 0; tl_end = 0x7FFFFFFF;
    const uint32_t n = ctx.r3.u32;
    if (part == 1) Wr32(base, 0x829E1348 + 56, n); else count_b.store(n);
    if (cmd) Wr32(base, cmd + 5444, Rd32(base, cmd + 5444) + n);
    CS_TRACE("job {} recorded {} draws [{}, {})", part, n, part == 1 ? 0 : mid.load(), part == 1 ? mid.load() : -1);
    ctx.r3.u64 = slot; ctx.lr = 0x8219164C;
    sub_8218DC50(ctx, base);             // end the command buffer
    CS_TRACE("job {} ended", part);
    if (part == 1) a_done.store(true, std::memory_order_release);
    return true;
}
// Frame hook: fps per mode, A/B switching.
void Frame() {
    if (!On()) return;
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    static auto mode_start = std::chrono::steady_clock::now();
    static uint64_t frames[2] = {}, frames_now = 0;
    static double secs[2] = {};
    ++frames_now;
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    const int m = Active() ? 1 : 0;
    frames[m] += frames_now; secs[m] += std::chrono::duration<double>(now - mode_start).count();
    frames_now = 0; mode_start = now; next = now + std::chrono::seconds(10);
    const uint64_t jc = join_b_count.exchange(0), jt = join_b_ticks.exchange(0);
    REXLOG_INFO("COLOUR SPLIT: this window {} | splits {} (skipped: {} shadows>2, {} small, {} pending) | no pool C "
                "memory {} | job B join wait avg {:.3f} ms | cumulative fps: split OFF {:.1f} ({:.0f} s), ON {:.1f} ({:.0f} s)",
                m ? "ON" : "OFF", splits.exchange(0), skipped_shadows.exchange(0), skipped_small.exchange(0),
                skipped_pending.exchange(0), no_memory.load(), jc ? jt * mesh_census::TickUs() / 1000.0 / jc : 0.0,
                secs[0] > 0 ? frames[0] / secs[0] : 0.0, secs[0], secs[1] > 0 ? frames[1] / secs[1] : 0.0, secs[1]);
    if (AbOn()) ab_active.store(!ab_active.load());
}
}  // namespace colour_split
// Command-buffer allocation: half B takes pool C (type 0).
PPC_FUNC(sub_82196A38) {
    if (!colour_split::tl_force_pool_c) { __imp__sub_82196A38(ctx, base); return; }
    ctx.r3.u64 = 0;
    __imp__sub_82196A38(ctx, base);  // picks a free pool C buffer, arms its context; r3 = handle
    const uint32_t h = ctx.r3.u32;
    if ((h & 0xC0000000u) == 0xC0000000u && (h & 0x3FFFFFFFu) < 2) {
        const uint32_t obj = 0x838F0960u + (h & 0x3FFFFFFFu) * 5456u;
        if (Rd32(base, obj + 5436) == 0)
            Wr32(base, obj + 5436, colour_split::cmd_mem[Rd32(base, 0x8370D78C) & 1]);
    }
}
// Command-buffer insert: after the colour job's buffer (A), join job B and insert its buffer.
PPC_FUNC(sub_82196BF8) {
    __imp__sub_82196BF8(ctx, base);
    const uint32_t hb = colour_split::pending_b.load();
    if (!hb || uint32_t(ctx.lr) != 0x82192B30) return;
    colour_split::CtxGuard g(ctx);
    CS_TRACE("inserted A, joining B");
    const uint64_t t0 = __rdtsc();
    ctx.r3.u64 = hb; ctx.r4.u64 = 0; ctx.lr = 0x82192B28;
    sub_826375A8(ctx, base);             // join job B
    colour_split::join_b_ticks.fetch_add(__rdtsc() - t0); colour_split::join_b_count.fetch_add(1);
    colour_split::pending_b.store(0);
    ctx.r3.u64 = Rd32(base, colour_split::slot_b); ctx.lr = 0x82192B30;
    __imp__sub_82196BF8(ctx, base);      // insert B right after A
    Wr32(base, 0x829E1348 + 56, Rd32(base, 0x829E1348 + 56) + colour_split::count_b.load());
    CS_TRACE("inserted B - split frame done");
    colour_split::trace.fetch_add(1);
}
// Hook dispatch used by the SR_RT_TIMED wrappers (sections 24 + 25).
inline bool sr_special_hooks() { return render_overlap::Point() || colour_split::On(); }
inline bool sr_special_before(PPCContext& ctx, uint8_t* base, uint32_t fn) {
    if (colour_split::On()) {
        if (fn == 0x826374B0 && uint32_t(ctx.lr) == 0x82191F78 && ctx.r5.u32 != 0) {
            if (colour_split::Submit(ctx, base)) return true;
        } else if (fn == 0x821915E8) {
            if (colour_split::Half(ctx, base)) return true;
        }
    }
    return render_overlap::Point() && render_overlap::Before(ctx, base, fn);
}
inline void sr_special_after(PPCContext& ctx, uint8_t* base, uint32_t fn) {
    if (render_overlap::Point()) render_overlap::After(ctx, base, fn);
}

#!/usr/bin/env python3
# Add the one-shot pr-diagnostic at the top of mc_charqRenderHandler.
import sys
path = sys.argv[1]
s = open(path, "r", encoding="utf-8", newline="").read()
had_crlf = "\r\n" in s
s = s.replace("\r\n", "\n")

OLD = '''static void mc_charqRenderHandler(const u32* r)
{
	// BODY-vs-HUD filter: only capture when this loc_8C1244B0 invocation came from the
	// body render (loc_8c0344d4) submit jsr. pr (caller return) must be 0x0C03487A.
	u32 pr = Sh4cntx.pr & SH4_AREA_MASK;
	if (pr != PC_BODY_SUBMIT_RET_M) return;'''

NEW = '''static void mc_charqRenderHandler(const u32* r)
{
	// DIAG (one-shot): prove the handler is REACHED + reveal the actual caller pr's, so
	// we can confirm/adjust the body-vs-HUD filter on live. Logs the first 16 distinct pr
	// values seen (area-masked + raw) regardless of the filter below.
	{
		static u32  s_prSeen[16]; static int s_prN = 0; static unsigned long s_reach = 0;
		u32 prm = Sh4cntx.pr & SH4_AREA_MASK;
		if (s_reach++ == 0)
			fprintf(stderr, "[CHARQ-RENDER] handler REACHED (pc=0x%08X) — collecting caller pr's\\n",
			        PC_CHARQ_SUBMIT);
		bool known = false; for (int i=0;i<s_prN;i++) if (s_prSeen[i]==prm) { known=true; break; }
		if (!known && s_prN < 16) {
			s_prSeen[s_prN++] = prm;
			fprintf(stderr, "[CHARQ-RENDER] caller pr=0x%08X (masked 0x%08X) %s\\n",
			        Sh4cntx.pr, prm, (prm==PC_BODY_SUBMIT_RET_M) ? "<== BODY (loc_8c0344d4)" : "");
		}
	}

	// BODY-vs-HUD filter: only capture when this loc_8C1244B0 invocation came from the
	// body render (loc_8c0344d4) submit jsr. pr (caller return) must be 0x0C03487A.
	u32 pr = Sh4cntx.pr & SH4_AREA_MASK;
	if (pr != PC_BODY_SUBMIT_RET_M) return;'''

c = s.count(OLD)
if c != 1:
    sys.stderr.write("expected 1 match, found %d\n" % c); sys.exit(2)
s = s.replace(OLD, NEW)
if had_crlf: s = s.replace("\n", "\r\n")
open(path, "w", encoding="utf-8", newline="").write(s)
sys.stderr.write("OK diag applied\n")

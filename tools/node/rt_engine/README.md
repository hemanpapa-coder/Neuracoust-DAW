# rt-engine — versioned copy

The engine that runs on a DSP node. The WORKING TREE is
`/Volumes/Program Dev/Linux DSP Server/rt_engine` — which is not under version
control — so this copy in DW's git is the only history the engine has. Edit
there, sync here, commit here.

Multi-module + sessions (2026-07-28): the engine loads several `--module x.so`
and routes each packet by an optional route block (module id + session id,
flag 0x2). A session is a private state instance, so two streams through one
module never share filter memories. No route block = module 0, session 0 —
bit-identical to the old single-module engine, which is what an old DAW sends.

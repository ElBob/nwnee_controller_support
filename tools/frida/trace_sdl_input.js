// Frida: count calls to SDL input APIs once per second, to see how the game reads
// keyboard and mouse (events vs. state polling). Useful for arbitration and gating work.
//   frida -q -n nwmain-linux -l tools/frida/trace_sdl_input.js --timeout 30
const names = [
  "SDL_PollEvent", "SDL_PeepEvents", "SDL_PumpEvents",
  "SDL_GetKeyboardState", "SDL_GetMouseState", "SDL_GetRelativeMouseState",
  "SDL_IsTextInputActive", "SDL_GL_SwapWindow",
];
function findExport(name) {
  // Frida 17 removed Module.findExportByName(null, ...); support both.
  if (typeof Module.findGlobalExportByName === "function") return Module.findGlobalExportByName(name);
  return Module.findExportByName(null, name);
}
const counts = {};
for (const n of names) {
  const addr = findExport(n);
  if (!addr) { console.log(`missing: ${n}`); continue; }
  counts[n] = 0;
  Interceptor.attach(addr, { onEnter() { counts[n]++; } });
}
setInterval(() => {
  console.log(JSON.stringify({ t: Date.now(), calls: counts }));
  for (const k in counts) counts[k] = 0;
}, 1000);

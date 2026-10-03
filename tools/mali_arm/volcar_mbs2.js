const SALIDA = SALIDA_RUTA;
function hookAll(m) {
  const deser = new NativeFunction(m.getExportByName("cmpbe_v2_deserialize_MBS2_to_C"), "uint64", ["pointer", "pointer", "pointer"]);
  Interceptor.attach(m.getExportByName("cmpbe_v2_compile_multiple_shaders"), {
    onEnter(a) { this.ctx = a[0]; this.n = a[1].toInt32(); this.out = a[10]; },
    onLeave(r) {
      if (r.toInt32() !== 0) { send("compilacion fallo " + r); return; }
      const prog = this.out.readPointer();
      for (let i = 0; i < this.n; i++) {
        const e = prog.add(i * 72);
        const datos = e.add(16).readPointer(), tam = e.add(24).readU32();
        send({ archivo: SALIDA + "_" + i + ".mbs2" }, datos.readByteArray(tam));
        const copia = Memory.alloc(72); Memory.copy(copia, e, 72);
        const out = Memory.alloc(8); out.writePointer(ptr(0));
        try {
          const st = deser(this.ctx, copia, out);
          const s = out.readPointer();
          if (st == 0 && !s.isNull()) { const txt = s.readUtf8String(); send({ archivo: SALIDA + "_" + i + ".h" }, s.readByteArray(txt.length)); }
          else send("deserialize = " + st);
        } catch (err) { send("deser error " + err); }
      }
    }
  });
}
let hecho = false;
Interceptor.attach(Process.getModuleByName("ntdll.dll").getExportByName("LdrLoadDll"), { onLeave(r) {
  if (hecho) return;
  const m = Process.findModuleByName("Mali-Gxx_r51p0-00rel0.dll");
  if (m) { hecho = true; try { hookAll(m); } catch (e) { send("ERROR " + e.stack); } }
}});

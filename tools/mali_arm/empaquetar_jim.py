"""Empaqueta el driver compilado por compilar_jim.sh como lo instala el juego (VulkanDriver.java): un zip con
meta.json + la librería. La librería se renombra a libvulkan_panfrost_nfsmw.so para no pisar otro PanVK instalado.

  python tools/mali_arm/empaquetar_jim.py [salida.zip]

Lee el zip que deja build.sh en out/jim/mesa/dist/ y escribe out/jim/panvk-nfsmw.zip (por defecto). En el teléfono
va en <carpeta del juego>/drivers/; el juego lo vuelve a copiar cuando cambian el tamaño o la fecha del zip.
"""
import glob, json, os, sys, zipfile

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(os.path.dirname(AQUI))
LIB = 'libvulkan_panfrost_nfsmw.so'


def main():
    salida = sys.argv[1] if len(sys.argv) > 1 else os.path.join(RAIZ, 'out', 'jim', 'panvk-nfsmw.zip')
    zips = sorted(glob.glob(os.path.join(RAIZ, 'out', 'jim', 'mesa', 'dist', '*.zip')), key=os.path.getmtime)
    if not zips:
        sys.exit('no hay zip en out/jim/mesa/dist (correr compilar_jim.sh)')
    with zipfile.ZipFile(zips[-1]) as f, zipfile.ZipFile(salida, 'w', zipfile.ZIP_DEFLATED) as o:
        meta = json.loads(f.read('meta.json'))
        original = meta['libraryName']
        meta['name'] += ' (nfsmw)'
        meta['libraryName'] = LIB
        o.writestr('meta.json', json.dumps(meta, indent=2))
        o.writestr(LIB, f.read(original))
        o.writestr('NOTICE.txt', f.read('NOTICE.txt'))
    print(zips[-1], '->', salida)


if __name__ == '__main__':
    main()

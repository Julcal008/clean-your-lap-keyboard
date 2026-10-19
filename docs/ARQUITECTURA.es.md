# Arquitectura

Este documento explica cómo está construido Clean Your Lap Keyboard. Está
pensado para que lo entienda cualquiera, aunque no conozca evdev, udev o GTK.
Todo lo que se describe aquí coincide con el código de `src/` y `helper/`.

## El problema

Los teclados de portátil no se pueden "apagar" desde una aplicación normal: las
teclas las gestiona el kernel. Lo que sí podemos hacer es tomar un **captura
exclusiva** del dispositivo evdev con el ioctl `EVIOCGRAB`. Mientras la captura
está activa, el kernel entrega los eventos de teclado solo a nosotros (y los
ignoramos), así que no se escribe nada.

Tomar esa captura requiere `CAP_SYS_ADMIN`, por lo que la aplicación de usuario
no puede hacerlo directamente. Esa es la única razón por la que existe un
pequeño helper privilegiado.

## Las dos mitades

```text
+------------------------------+          +-----------------------------+
|  cylk  (usuario normal)      |          |  cylk-helper  (root)        |
|                              |  stdin   |                             |
|  Interfaz GTK4 / libadwaita  | -------> |  abre /dev/input/eventN     |
|  detección (solo lectura)    |  stdout  |  EVIOCGRAB en cada uno      |
|  máquina de estados + tiempo | <------- |  baja privilegios           |
+------------------------------+          +-----------------------------+
              ^                                        |
              | D-Bus (logind)                         | evdev
              |                                        v
        ¿pantalla bloqueada?                    /dev/input/event*
        ¿sesión inactiva?
```

La interfaz nunca abre un dispositivo de entrada. El helper nunca dibuja nada y
nunca lee (ni registra) eventos: solo captura y espera.

## Archivos fuente

| Archivo | Responsabilidad |
| --- | --- |
| `src/main.c` | Opciones de línea de comandos, gettext y arranque. |
| `src/window.c` | Toda la interfaz: lista de teclados, cuenta atrás, limpieza, fin. |
| `src/device.c` / `src/device.h` | Descubrir y clasificar teclados y auxiliares. |
| `src/companion-keys.h` | La única lista de teclas "bloqueadas". |
| `src/config.c` / `src/config.h` | Guardar duración y teclado elegido. |
| `src/helper.c` / `src/helper.h` | Lanzar el helper y hablar el protocolo. |
| `src/session.c` / `src/session.h` | Vigilar logind (bloqueo / suspensión / inactividad). |
| `helper/cylk-helper.c` | El helper privilegiado que captura y libera. |
| `tests/test-device.c` | Pruebas unitarias de la lógica de dispositivos. |

## El modelo de dispositivos

`CylkDevice` describe un nodo evdev. Cada uno es:

- un **teclado** (`CYLK_DEVICE_KEYBOARD`): un dispositivo con capacidades de
  teclado completo. Son los que el usuario puede elegir para limpiar.
- un **auxiliar** (`CYLK_DEVICE_COMPANION`): un nodo que emite teclas multimedia
  (volumen, silencio, brillo, radios) o botones físicos (encendido, suspensión).
  Se capturan junto con el teclado.

### Cómo se reconoce un teclado

`cylk_device_scan_all()` recorre `/sys/class/input` con libudev y, para cada
`/dev/input/eventN`, lee los mapas de capacidades de sysfs
(`device/capabilities/key`, `.../ev`, `.../rel`, `.../abs`). Un dispositivo es un
teclado completo si su mapa de teclas tiene `KEY_A`, `KEY_Z`, `KEY_0`,
`KEY_ENTER`, `KEY_SPACE` y `KEY_LEFTSHIFT`, y su nombre no está en la lista
excluida (touchpads, video, botones de encendido, ...).

### Cómo se adivina el teclado interno

`cylk_device_score()` suma puntos para el bus i8042/serio y para el nombre
clásico "AT Translated Set 2 keyboard", y resta para USB. Una puntuación de 100 o
más marca el dispositivo como **interno**. El usuario siempre puede cambiarlo; la
elección se recuerda en `~/.config/clean-your-lap-keyboard/`.

### Cómo se encuentran los auxiliares

`cylk_device_scan_companions()` conserva los auxiliares que comparten hardware
con el teclado elegido (`cylk_device_is_companion_candidate()`):

- **Misma interfaz USB**: el `ID_PATH` de los dos nodos solo difiere en el último
  índice, por ejemplo `...-usb-0:2:1.0` (teclado) y `...-usb-0:2:1.1` (control
  de consumo). Así se manejan los teclados externos: sus teclas multimedia solo
  se bloquean si se seleccionó ese teclado.
- **Teclado integrado**: cualquier dispositivo ACPI/plataforma se considera parte
  del portátil, lo que cubre `Video Bus` (brillo), `ThinkPad Extra Buttons`
  (volumen, micrófono, radios) y los nodos `Power Button` / `Sleep Button`.

Un puntero, un segundo teclado o un dispositivo sin teclas multimedia/de botón
nunca se elige como auxiliar.

### Por qué se incluye el botón de encendido

El botón de encendido vive en su propio dispositivo ACPI (`event0`, `event3`,
...). Si no se captura, se puede pulsar sin querer al limpiar y el equipo se
apagaría a mitad de la operación. Capturarlo es seguro porque mantener el botón
pulsado varios segundos sigue forzando un apagado por **hardware** que gestiona
el controlador embebido, sin depender del sistema operativo.

## La captura

Cuando el usuario pulsa **Empezar limpieza**:

1. `src/window.c` pide a `cylk_device_scan_companions()` las rutas extra.
2. Envía `GRAB <ruta> <hold_ms> <max_segundos> <auxiliar> ...` al helper.
3. `helper/cylk-helper.c` abre cada ruta, valida cada una con
   `is_full_keyboard()` / `is_companion_device()`, espera a que se suelten todas
   las teclas y aplica `EVIOCGRAB` en todas.
4. Después baja de root, activa `PR_SET_NO_NEW_PRIVS` y limpia el flag de volcado.
5. Informa `OK` y entra en un bucle mínimo que solo espera `RELEASE`, EOF, una
   **Esc** mantenida o la duración máxima.

El teclado se libera cuando ocurre cualquiera de estas cosas:

- termina la duración elegida (interfaz + límite duro en el helper);
- el usuario pulsa **Detener**;
- el usuario mantiene **Esc** durante 2 segundos (liberación de emergencia);
- se cierra la ventana o termina la aplicación;
- logind informa de bloqueo, suspensión o sesión inactiva;
- se cierra la entrada estándar del helper (el kernel cierra sus descriptores).

Cerrar los descriptores basta para que el kernel elimine la captura, así que un
fallo de la interfaz nunca deja el teclado desactivado.

## Protocolo del helper

Líneas separadas por tabuladores por stdin/stdout:

```text
UI -> helper : GRAB <ruta> <hold_ms> <max_segundos> [<auxiliar> ...]
UI -> helper : RELEASE
UI -> helper : QUIT
helper -> UI : READY <versión-de-protocolo>
helper -> UI : OK <nombre-del-dispositivo>
helper -> UI : RELEASED <motivo>
helper -> UI : ERR <código> <mensaje>
```

`READY 2` es la versión actual. La versión permite que la interfaz detecte un
helper antiguo instalado junto a una aplicación nueva y se niegue a continuar en
lugar de capturar solo el teclado principal sin avisar.

## Añadir una tecla bloqueada

1. Añade el macro `KEY_*` a `CYLK_COMPANION_KEY_LIST` (multimedia) o a
   `CYLK_BUTTON_KEY_LIST` (botones físicos) en `src/companion-keys.h`.
2. Añade una prueba en `tests/test-device.c` si el comportamiento no es obvio.
3. Ejecuta `meson test -C build`.

Como la interfaz (vía `device.c`) y el helper (vía `cylk-helper.c`) incluyen el
mismo encabezado, la tecla queda reconocida en ambos lados.

## Compromisos conocidos

- Capturar `ThinkPad Extra Buttons` también bloquea Fn+suspender, los
  interruptores WLAN/Bluetooth, la batería y las notificaciones **durante la
  limpieza**. Vuelven en cuanto se libera la captura.
- Las teclas que el firmware gestiona por completo en hardware (sin emitir
  ningún evento evdev) no se pueden bloquear desde el espacio de usuario.
- La detección automática es heurística; el selector manual y `cylk --list-all`
  existen para los casos en que se equivoca.

# Smooth Taskbar Auto-Hide

Reemplaza el comportamiento instantáneo de ocultación automática de la barra de tareas por una animación deslizante y fluida. Se mueve la propia ventana de la barra de tareas —no se desvanece—, por lo que conserva el aspecto visual nativo de Windows.

> **Requisito:** activa en Windows la opción **«Ocultar automáticamente la barra de tareas»**. Si está desactivada, el mod no interviene y se mantiene el comportamiento normal de la barra.

## Cómo funciona

El mod intercepta las decisiones de Explorer para mostrar u ocultar la barra de tareas (`SetWindowPos` en `Shell_TrayWnd` y `Shell_SecondaryTrayWnd`) y las convierte en una animación con aceleración y desaceleración suaves.

También incorpora detección de movimiento del cursor en el borde inferior de la pantalla y, de forma opcional, puede mostrar la barra cuando se minimiza una ventana. Los activadores se deshabilitan mientras hay una aplicación en pantalla completa en primer plano.

## Dirección de la animación

La opción **Automática** anima la barra hacia el borde donde está acoplada. Por ejemplo, si está en la parte inferior, se desliza hacia abajo al ocultarse. Forzar una dirección distinta hace que la barra cruce la pantalla hacia ese lado y normalmente no es recomendable.

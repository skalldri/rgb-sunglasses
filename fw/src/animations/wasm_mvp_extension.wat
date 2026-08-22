(module
  (import "rgbx_mvp" "fill" (func $fill (param i32)))

  ;; Alternate the whole panel between cyan and magenta every 500 ms.
  (func (export "rgbx_tick") (param $elapsed_ms i32)
    local.get $elapsed_ms
    i32.const 500
    i32.div_u
    i32.const 1
    i32.and
    if
      i32.const 0xff00ff
      call $fill
    else
      i32.const 0x00ffff
      call $fill
    end))

import tgl

width, height = tgl.screen_size()
surface = tgl.Surface(420, 300)
surface.vgradient(0, 0, 420, 300, 0x081C15, 0x174D35)
surface.fill(35, 35, 350, 230, 0x1E8A4C)
surface.blend(35, 35, 350, 230, 0x000000, 65)
surface.line(35, 35, 384, 264, 0xFFE060)
surface.line(384, 35, 35, 264, 0xFFE060)
surface.disc(210, 150, 52, 0xF4F1DE)

x = (width - 420) // 2
y = (height - 300) // 2
while surface.present(x, y) == 0:
    pass
surface.close()
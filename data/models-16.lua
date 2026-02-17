TileSize = 16
SpriteSize = 20
ShadowSize = 21
MainFont = "MicroHei/wqy-microhei.ttc"
LevelMenuFont = MainFont
-- Alternative fonts are e.g.:
--   MainFont = "DejaVuSansCondensed.ttf"
--   MainFont = "vera_sans.ttf"

DefineFont ("timefont", MainFont, 18, "timefont", 180, 180, 180)
DefineFont ("smallalternative", MainFont, 7, "menufont")
DefineFont ("smallalternative_good", MainFont, 7, "menufont", 80, 220, 80)
DefineFont ("smallalternative_normal", MainFont, 7, "menufont", 255, 180, 0)
DefineFont ("smallalternative_bad", MainFont, 7, "menufont", 255, 90, 90)
DefineFont ("smallalternative_selected", MainFont, 7, "menufont", 120, 200, 255)
DefineFont ("modesfont", MainFont, 9, "menufont", 70, 120, 255)
DefineFont ("menufont", MainFont, 9, "menufont")
DefineFont ("levelmenu", LevelMenuFont, 9, "levelmenu")
DefineFont ("menufontsel", MainFont, 10, "menufont", 180, 180, 180)
DefineFont ("statusbarfont", MainFont, 7, "dreamorp24")

dofile(FindDataFile("models-2d.lua"))

TileSize = 48
SpriteSize = 60
ShadowSize = 61
MainFont = "MicroHei/wqy-microhei.ttc"
LevelMenuFont = "" -- will load levelmenu.bmf instead
-- Alternative fonts are e.g.:
--   MainFont = "DejaVuSansCondensed.ttf"
--   MainFont = "vera_sans.ttf"

DefineFont ("timefont", MainFont, 40, "timefont", 180, 180, 180)
DefineFont ("smallalternative", MainFont, 14, "menufont")
DefineFont ("smallalternative_good", MainFont, 14, "menufont", 80, 220, 80)
DefineFont ("smallalternative_normal", MainFont, 14, "menufont", 255, 180, 0)
DefineFont ("smallalternative_bad", MainFont, 14, "menufont", 255, 90, 90)
DefineFont ("smallalternative_selected", MainFont, 14, "menufont", 120, 200, 255)
DefineFont ("modesfont", MainFont, 16, "menufont", 70, 120, 255)
DefineFont ("menufont", MainFont, 16, "menufont")
DefineFont ("levelmenu", LevelMenuFont, 16, "levelmenu")
DefineFont ("menufontsel", MainFont, 16, "menufont", 180, 180, 180)
DefineFont ("statusbarfont", MainFont, 24, "dreamorp24")

dofile(FindDataFile("models-2d.lua"))

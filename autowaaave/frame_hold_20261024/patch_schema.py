"""Strict video patch wire schema shared with the controller's version 4 fields."""
FIELDS=[('enc0', 0, 127), ('enc1', 0, 127), ('enc2', 0, 127), ('enc3', 0, 127), ('enc4', 0, 127), ('enc5', 0, 127), ('enc6', 0, 127), ('enc7', 0, 127), ('enc8', 0, 127), ('enc9', 0, 127), ('enc10', 0, 127), ('enc11', 0, 127), ('enc12', 0, 127), ('enc13', 0, 127), ('enc14', 0, 127), ('enc15', 0, 127), ('enc16', 0, 127), ('enc17', 0, 127), ('ctrl0', 0, 1), ('ctrl1', 0, 1), ('ctrl2', 0, 1), ('ctrl3', 0, 1), ('ctrl4', 0, 1), ('ctrl5', 0, 1), ('ctrl6', 0, 1), ('ctrl7', 0, 1), ('func0', 0, 3), ('func1', 0, 3), ('func2', 0, 3), ('func3', 0, 3), ('func4', 0, 3), ('func5', 0, 3), ('func6', 0, 3), ('func7', 0, 3), ('react', -1, 2), ('lumaKey', 0, 127), ('lumaHigh', 0, 127), ('lumaLowEn', 0, 1), ('lumaHiEn', 0, 1), ('renderLumaInvert', 0, 1), ('renderMirror', 0, 1), ('mix', 0, 127), ('chA', 0, 25), ('chB', 0, 25), ('lumaSrc', 0, 26), ('lumaTrack', 0, 1), ('mixMode', 0, 2), ('clip', 0, 63), ('loop', 0, 1), ('srcHue', 0, 127), ('srcSat', 0, 127), ('srcVal', 0, 127), ('srcShader', 0, 127), ('mosh', 0, 127), ('chroma', 0, 127), ('crush', 0, 127), ('pixel', 0, 127), ('bleed', 0, 127), ('align', 0, 127), ('ghost', 0, 127), ('wobble', 0, 127), ('overdrive', 0, 127), ('hold', 0, 127), ('houseCard', 0, 1), ('houseLogo', 0, 1), ('mainLogo', 0, 1), ('patchVersion', 1, 4), ('slot', 0, 31)]
LEGACY_KEYS=['enc0', 'enc1', 'enc2', 'enc3', 'enc4', 'enc5', 'enc6', 'enc7', 'enc8', 'enc9', 'enc10', 'enc11', 'enc12', 'enc13', 'enc14', 'enc15', 'enc16', 'enc17', 'ctrl0', 'ctrl1', 'ctrl2', 'ctrl3', 'ctrl4', 'ctrl5', 'ctrl6', 'ctrl7', 'func0', 'func1', 'func2', 'func3', 'func4', 'func5', 'func6', 'func7', 'react', 'lumaKey', 'lumaHigh', 'lumaLowEn', 'lumaHiEn', 'renderLumaInvert', 'renderMirror', 'mix', 'chA', 'chB', 'lumaSrc', 'lumaTrack', 'mixMode', 'clip', 'loop', 'srcHue', 'srcSat', 'srcVal', 'srcShader', 'mosh', 'chroma', 'crush', 'pixel', 'houseCard', 'houseLogo', 'mainLogo', 'patchVersion', 'slot']
V3_KEYS=[key for key,_,_ in FIELDS if key!='hold']
def video_patch(values,slot):
    if type(slot) is not int or not 0<=slot<32:raise ValueError('slot')
    if not isinstance(values,list):raise ValueError('field count')
    if len(values)==len(FIELDS)-1:
        version=values[-2]
        if version>3:raise ValueError('field count')
        values=values[:len(V3_KEYS)-5]+[0]+values[len(V3_KEYS)-5:]
    if len(values)!=len(FIELDS):raise ValueError('field count')
    patch={}
    for (key,lo,hi),value in zip(FIELDS,values):
        if type(value) is not int or not lo<=value<=hi:raise ValueError(key)
        patch[key]=value
    if patch['slot']!=slot or patch['lumaKey']>patch['lumaHigh']:raise ValueError('slot/threshold pair')
    return patch

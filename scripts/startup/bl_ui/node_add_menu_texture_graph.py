# SPDX-FileCopyrightText: 2026 Weld 3D Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Add and Swap menus of the Texture Graph editor. The graphs run on the compositor engine, so
# most nodes are the compositor's and the shared texture, color and math nodes.

from bl_ui import node_add_menu
from bpy.app.translations import (
    contexts as i18n_contexts,
)


class NODE_MT_texture_graph_node_input_base(node_add_menu.NodeMenu):
    bl_label = "Input"

    def draw(self, _context):
        layout = self.layout
        self.draw_menu(layout, path="Input/Constant")
        layout.separator()
        self.node_operator(layout, "NodeGroupInput")
        self.node_operator(layout, "CompositorNodeImage")
        self.node_operator(layout, "CompositorNodeImageInfo")
        self.node_operator(layout, "CompositorNodeImageCoordinates")
        layout.separator()
        self.node_operator_with_outputs(_context, layout, "CompositorNodeSceneTime", ["Frame", "Seconds"])

        self.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_texture_graph_node_input_constant_base(node_add_menu.NodeMenu):
    bl_label = "Constant"
    bl_translation_context = i18n_contexts.id_nodetree
    menu_path = "Input/Constant"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "FunctionNodeInputBool")
        self.node_operator(layout, "CompositorNodeRGB")
        self.node_operator(layout, "FunctionNodeInputInt")
        self.node_operator(layout, "ShaderNodeValue")
        self.node_operator(layout, "FunctionNodeInputVector")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_output_base(node_add_menu.NodeMenu):
    bl_label = "Output"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "NodeGroupOutput")
        self.node_operator(layout, "CompositorNodeViewer")

        self.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_texture_graph_node_color_base(node_add_menu.NodeMenu):
    bl_label = "Color"

    def draw(self, context):
        layout = self.layout
        self.draw_menu(layout, path="Color/Adjust")
        self.draw_menu(layout, path="Color/Histogram")
        self.draw_menu(layout, path="Color/Palette")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeBlend")
        self.node_operator(layout, "TextureGraphNodeHeightBlend")
        self.color_mix_node(context, layout)
        self.node_operator(layout, "TextureGraphNodeLinearBurn")
        self.node_operator(layout, "TextureGraphNodeDissolve")
        self.node_operator(layout, "CompositorNodeAlphaOver")
        self.node_operator(layout, "CompositorNodeSetAlpha")
        layout.separator()
        self.node_operator(layout, "CompositorNodeCombineColor")
        self.node_operator(layout, "CompositorNodeSeparateColor")
        self.node_operator(layout, "TextureGraphNodeChannelShuffle")
        self.node_operator(layout, "TextureGraphNodeChannelMixer")
        layout.separator()
        self.node_operator(layout, "ShaderNodeValToRGB")
        self.node_operator(layout, "CompositorNodeRGBToBW")
        self.node_operator(layout, "TextureGraphNodeGrayscaleConversion")
        self.node_operator(layout, "TextureGraphNodeChrominanceExtract")
        self.node_operator(layout, "TextureGraphNodeColorToMask")
        self.node_operator(layout, "CompositorNodeInvert")

        self.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_texture_graph_node_color_adjust_base(node_add_menu.NodeMenu):
    bl_label = "Adjust"
    menu_path = "Color/Adjust"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeLevels")
        self.node_operator(layout, "CompositorNodeCurveRGB")
        self.node_operator(layout, "CompositorNodeHueSat")
        self.node_operator(layout, "CompositorNodeBrightContrast")
        self.node_operator(layout, "CompositorNodeColorBalance")
        self.node_operator(layout, "CompositorNodeExposure")
        self.node_operator(layout, "ShaderNodeGamma")
        self.node_operator(layout, "CompositorNodeHueCorrect")
        self.node_operator(layout, "CompositorNodePosterize")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeContrastLuminosity")
        self.node_operator(layout, "TextureGraphNodeReplaceColor")
        self.node_operator(layout, "TextureGraphNodeReplaceColorRange")
        self.node_operator(layout, "TextureGraphNodeColorMatch")
        self.node_operator(layout, "TextureGraphNodeHaldCLUT")
        self.node_operator(layout, "TextureGraphNodeQuantizeColorSimple")
        self.node_operator(layout, "TextureGraphNodeHDRRangeViewer")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_color_histogram_base(node_add_menu.NodeMenu):
    bl_label = "Histogram"
    menu_path = "Color/Histogram"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeAutoLevels")
        self.node_operator(layout, "TextureGraphNodeHistogramEqualize")
        self.node_operator(layout, "TextureGraphNodeHistogramRange")
        self.node_operator(layout, "TextureGraphNodeHistogramShift")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeHistogramScan")
        self.node_operator(layout, "TextureGraphNodeHistogramScanNonUniform")
        self.node_operator(layout, "TextureGraphNodeHistogramSelect")
        self.node_operator(layout, "TextureGraphNodeThreshold")
        self.node_operator(layout, "TextureGraphNodeQuantizeGrayscale")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeHistogramCompute")
        self.node_operator(layout, "TextureGraphNodeHistogramRender")
        self.node_operator(layout, "TextureGraphNodeMinMax")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_color_palette_base(node_add_menu.NodeMenu):
    bl_label = "Palette"
    menu_path = "Color/Palette"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeQuantizeColor")
        self.node_operator(layout, "TextureGraphNodeCreatePalette")
        self.node_operator(layout, "TextureGraphNodeModifyPalette")
        self.node_operator(layout, "TextureGraphNodeApplyPalette")
        self.node_operator(layout, "TextureGraphNodeViewPalette")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeIDToMask")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_filter_effects_base(node_add_menu.NodeMenu):
    bl_label = "Effects"
    menu_path = "Filter/Effects"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeAmbientOcclusion")
        self.node_operator(layout, "TextureGraphNodeBevel")
        self.node_operator(layout, "TextureGraphNodeCrossSection")
        self.node_operator(layout, "TextureGraphNodeShadows")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeCurvature")
        self.node_operator(layout, "TextureGraphNodeCurvatureSmooth")
        self.node_operator(layout, "TextureGraphNodeCurvatureSobel")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeDiffusionColor")
        self.node_operator(layout, "TextureGraphNodeDiffusionGrayscale")
        self.node_operator(layout, "TextureGraphNodeDiffusionUV")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeEmbossWithGloss")
        self.node_operator(layout, "TextureGraphNodeUberEmboss")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeMosaic")
        self.node_operator(layout, "TextureGraphNodeSummedAreaTable")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_filter_shape_effects_base(node_add_menu.NodeMenu):
    bl_label = "Shape Effects"
    menu_path = "Filter/Shape Effects"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeExtendShape")
        self.node_operator(layout, "TextureGraphNodeShapeDropShadow")
        self.node_operator(layout, "TextureGraphNodeShapeGlow")
        self.node_operator(layout, "TextureGraphNodeShapeStroke")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_filter_flood_fill_base(node_add_menu.NodeMenu):
    bl_label = "Flood Fill"
    menu_path = "Filter/Flood Fill"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeFloodFill")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeFloodFillMapper")
        self.node_operator(layout, "TextureGraphNodeFloodFillToBBoxSize")
        self.node_operator(layout, "TextureGraphNodeFloodFillToColor")
        self.node_operator(layout, "TextureGraphNodeFloodFillToGradient")
        self.node_operator(layout, "TextureGraphNodeFloodFillToIndex")
        self.node_operator(layout, "TextureGraphNodeFloodFillToPosition")
        self.node_operator(layout, "TextureGraphNodeFloodFillToRandomColor")
        self.node_operator(layout, "TextureGraphNodeFloodFillToRandomGrayscale")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_filter_normal_map_base(node_add_menu.NodeMenu):
    bl_label = "Normal Map"
    menu_path = "Filter/Normal Map"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeNormalSobel")
        self.node_operator(layout, "TextureGraphNodeHeightToNormalWorldUnits")
        self.node_operator(layout, "TextureGraphNodeHeightNormalBlender")
        self.node_operator(layout, "TextureGraphNodeNormalToHeight")
        self.node_operator(layout, "TextureGraphNodeNormalToHeightHQ")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeNormalBlend")
        self.node_operator(layout, "TextureGraphNodeNormalCombine")
        self.node_operator(layout, "TextureGraphNodeNormalUncombine")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeNormalInvert")
        self.node_operator(layout, "TextureGraphNodeNormalNormalize")
        self.node_operator(layout, "TextureGraphNodeNormalTransform")
        self.node_operator(layout, "TextureGraphNodeNormalVectorRotation")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeFacingNormal")
        self.node_operator(layout, "TextureGraphNodeBentNormal")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_transform_tiling_base(node_add_menu.NodeMenu):
    bl_label = "Tiling"
    menu_path = "Transform/Tiling"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeMakeItTilePatch")
        self.node_operator(layout, "TextureGraphNodeMakeItTilePhoto")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeSafeTransform")
        self.node_operator(layout, "TextureGraphNodeNoiseUpscale1")
        self.node_operator(layout, "TextureGraphNodeNoiseUpscale2")
        self.node_operator(layout, "TextureGraphNodeNoiseUpscale3")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_filter_base(node_add_menu.NodeMenu):
    bl_label = "Filter"

    def draw(self, context):
        layout = self.layout
        self.draw_menu(layout, path="Filter/Blur")
        self.draw_menu(layout, path="Filter/Effects")
        self.draw_menu(layout, path="Filter/Flood Fill")
        self.draw_menu(layout, path="Filter/Normal Map")
        self.draw_menu(layout, path="Filter/Shape Effects")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeEdgeDetect")
        self.node_operator(layout, "TextureGraphNodeNonUniformEdgeDetect")
        self.node_operator(layout, "TextureGraphNodeHeightToNormal")
        self.node_operator(layout, "TextureGraphNodeDistance")
        self.node_operator(layout, "TextureGraphNodeEmboss")
        self.node_operator(layout, "TextureGraphNodeSharpen")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeHighpass")
        self.node_operator(layout, "TextureGraphNodeLuminanceHighpass")
        self.node_operator(layout, "TextureGraphNodeLightingCancelHigh")
        self.node_operator(layout, "TextureGraphNodeLightingCancelLow")
        self.node_operator(layout, "TextureGraphNodeHeightFrequencies")
        layout.separator()
        self.node_operator_with_searchable_enum_socket(
            context, layout, "CompositorNodeFilter", "Type", [
                "Soften", "Box Sharpen", "Diamond Sharpen", "Laplace", "Sobel", "Prewitt", "Kirsch", "Shadow",
            ],
        )
        self.node_operator(layout, "CompositorNodeDilateErode")
        self.node_operator(layout, "CompositorNodeDespeckle")
        self.node_operator(layout, "CompositorNodeKuwahara")
        self.node_operator(layout, "CompositorNodePixelate")
        self.node_operator(layout, "CompositorNodeMaskToSDF")
        self.node_operator(layout, "CompositorNodeInpaint")

        self.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_texture_graph_node_filter_blur_base(node_add_menu.NodeMenu):
    bl_label = "Blur"
    menu_path = "Filter/Blur"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "CompositorNodeBlur")
        self.node_operator(layout, "CompositorNodeDBlur")
        self.node_operator(layout, "CompositorNodeBilateralblur")
        self.node_operator(layout, "CompositorNodeBokehBlur")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeAnisotropicBlur")
        self.node_operator(layout, "TextureGraphNodeNonUniformBlur")
        self.node_operator(layout, "TextureGraphNodeRadialBlur")
        self.node_operator(layout, "TextureGraphNodeSlopeBlur")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeMedianFilter")
        self.node_operator(layout, "TextureGraphNodeMLVFilter")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_texture_noise_base(node_add_menu.NodeMenu):
    bl_label = "Noise"
    menu_path = "Texture/Noise"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodePerlinNoise")
        self.node_operator(layout, "TextureGraphNodeFractalSumBase")
        self.node_operator(layout, "TextureGraphNodeFractalSum1")
        self.node_operator(layout, "TextureGraphNodeFractalSum2")
        self.node_operator(layout, "TextureGraphNodeFractalSum3")
        self.node_operator(layout, "TextureGraphNodeFractalSum4")
        self.node_operator(layout, "TextureGraphNodeClouds1")
        self.node_operator(layout, "TextureGraphNodeClouds2")
        self.node_operator(layout, "TextureGraphNodeClouds3")
        self.node_operator(layout, "TextureGraphNodePlasma")
        self.node_operator(layout, "TextureGraphNodeCreased")
        self.node_operator(layout, "TextureGraphNodeFluid")
        self.node_operator(layout, "TextureGraphNodeLiquid")
        self.node_operator(layout, "TextureGraphNodeGaussianNoise")
        self.node_operator(layout, "TextureGraphNodeWhiteNoise")
        self.node_operator(layout, "TextureGraphNodeWhiteNoiseFast")
        self.node_operator(layout, "TextureGraphNodeBlueNoiseFast")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeAnisotropicNoise")
        self.node_operator(layout, "TextureGraphNodeDirectionalNoise1")
        self.node_operator(layout, "TextureGraphNodeDirectionalNoise2")
        self.node_operator(layout, "TextureGraphNodeDirectionalNoise3")
        self.node_operator(layout, "TextureGraphNodeDirectionalNoise4")
        self.node_operator(layout, "TextureGraphNodeDirectionalScratches")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeCells1")
        self.node_operator(layout, "TextureGraphNodeCells2")
        self.node_operator(layout, "TextureGraphNodeCells3")
        self.node_operator(layout, "TextureGraphNodeCells4")
        self.node_operator(layout, "TextureGraphNodeVoronoi")
        self.node_operator(layout, "TextureGraphNodeVoronoiFractal")
        self.node_operator(layout, "TextureGraphNodeCrystal1")
        self.node_operator(layout, "TextureGraphNodeCrystal2")
        self.node_operator(layout, "TextureGraphNodeCaustics")
        self.node_operator(layout, "TextureGraphNodeMicroscopeView")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeBnWSpots1")
        self.node_operator(layout, "TextureGraphNodeBnWSpots2")
        self.node_operator(layout, "TextureGraphNodeBnWSpots3")
        self.node_operator(layout, "TextureGraphNodeGaussianSpots1")
        self.node_operator(layout, "TextureGraphNodeGaussianSpots2")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_texture_grunge_base(node_add_menu.NodeMenu):
    bl_label = "Grunge"
    menu_path = "Texture/Grunge"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeGrungeGalvanicLarge")
        self.node_operator(layout, "TextureGraphNodeGrungeGalvanicSmall")
        self.node_operator(layout, "TextureGraphNodeGrungeLeaks")
        self.node_operator(layout, "TextureGraphNodeGrungeLeakyPaint")
        self.node_operator(layout, "TextureGraphNodeGrungeScratchesFine")
        self.node_operator(layout, "TextureGraphNodeGrungeScratchesRough")
        self.node_operator(layout, "TextureGraphNodeGrungeScratchesDirty")
        self.node_operator(layout, "TextureGraphNodeGrungeShavings")
        self.node_operator(layout, "TextureGraphNodeMessyFibers1")
        self.node_operator(layout, "TextureGraphNodeMessyFibers2")
        self.node_operator(layout, "TextureGraphNodeMessyFibers3")
        self.node_operator(layout, "TextureGraphNodeFur1")
        self.node_operator(layout, "TextureGraphNodeFur2")
        self.node_operator(layout, "TextureGraphNodeFur3")
        self.node_operator(layout, "TextureGraphNodeWaveform1")
        self.node_operator(layout, "TextureGraphNodeDirtGradient")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeGrungeConcrete")
        self.node_operator(layout, "TextureGraphNodeGrungeDamas")
        self.node_operator(layout, "TextureGraphNodeGrungeRoughDirty")
        self.node_operator(layout, "TextureGraphNodeGrungeRustFine")
        self.node_operator(layout, "TextureGraphNodeGrungeSpots")
        self.node_operator(layout, "TextureGraphNodeGrungeSpotsDirty")
        self.node_operator(layout, "TextureGraphNodeGrungeSplashesDusty")
        self.node_operator(layout, "TextureGraphNodeMoistureNoise1")
        self.node_operator(layout, "TextureGraphNodeMoistureNoise2")
        self.node_operator(layout, "TextureGraphNodeDirt1")
        self.node_operator(layout, "TextureGraphNodeDirt2")
        self.node_operator(layout, "TextureGraphNodeDirt3")
        self.node_operator(layout, "TextureGraphNodeDirt4")
        self.node_operator(layout, "TextureGraphNodeDirt5")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_texture_grunge_maps_base(node_add_menu.NodeMenu):
    bl_label = "Grunge Maps"
    menu_path = "Texture/Grunge Maps"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeGrungeMap001")
        self.node_operator(layout, "TextureGraphNodeGrungeMap002")
        self.node_operator(layout, "TextureGraphNodeGrungeMap003")
        self.node_operator(layout, "TextureGraphNodeGrungeMap004")
        self.node_operator(layout, "TextureGraphNodeGrungeMap005")
        self.node_operator(layout, "TextureGraphNodeGrungeMap006")
        self.node_operator(layout, "TextureGraphNodeGrungeMap007")
        self.node_operator(layout, "TextureGraphNodeGrungeMap008")
        self.node_operator(layout, "TextureGraphNodeGrungeMap009")
        self.node_operator(layout, "TextureGraphNodeGrungeMap010")
        self.node_operator(layout, "TextureGraphNodeGrungeMap011")
        self.node_operator(layout, "TextureGraphNodeGrungeMap012")
        self.node_operator(layout, "TextureGraphNodeGrungeMap013")
        self.node_operator(layout, "TextureGraphNodeGrungeMap014")
        self.node_operator(layout, "TextureGraphNodeGrungeMap015")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_texture_patterns_base(node_add_menu.NodeMenu):
    bl_label = "Patterns"
    menu_path = "Texture/Patterns"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeAlveolus")
        self.node_operator(layout, "TextureGraphNodeBrick1")
        self.node_operator(layout, "TextureGraphNodeBrick2")
        self.node_operator(layout, "TextureGraphNodeBrickGenerator")
        self.node_operator(layout, "TextureGraphNodeChecker1")
        self.node_operator(layout, "TextureGraphNodeStripes")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeFibers1")
        self.node_operator(layout, "TextureGraphNodeFibers2")
        self.node_operator(layout, "TextureGraphNodeMesh1")
        self.node_operator(layout, "TextureGraphNodeMesh2")
        self.node_operator(layout, "TextureGraphNodeWeave1")
        self.node_operator(layout, "TextureGraphNodeWeave2")
        self.node_operator(layout, "TextureGraphNodeWeaveGenerator")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeGaussian1")
        self.node_operator(layout, "TextureGraphNodeGaussian2")
        self.node_operator(layout, "TextureGraphNodeTriangleGrid")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_texture_gradients_base(node_add_menu.NodeMenu):
    bl_label = "Gradients"
    menu_path = "Texture/Gradients"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeGradientLinear1")
        self.node_operator(layout, "TextureGraphNodeGradientLinear2")
        self.node_operator(layout, "TextureGraphNodeGradientLinear3")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeGradientAxial")
        self.node_operator(layout, "TextureGraphNodeGradientAxialReflected")
        self.node_operator(layout, "TextureGraphNodeGradientCircular")
        self.node_operator(layout, "TextureGraphNodeGradientRadial")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_texture_shapes_base(node_add_menu.NodeMenu):
    bl_label = "Shapes"
    menu_path = "Texture/Shapes"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeShape")
        self.node_operator(layout, "TextureGraphNodePolygon1")
        self.node_operator(layout, "TextureGraphNodePolygon2")
        self.node_operator(layout, "TextureGraphNodeStar")
        self.node_operator(layout, "TextureGraphNodeStarburst")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeShapeExtrude")
        self.node_operator(layout, "TextureGraphNodeHeightExtrude")
        self.node_operator(layout, "TextureGraphNodePanoramaShape")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_texture_scatter_base(node_add_menu.NodeMenu):
    bl_label = "Scatter"
    menu_path = "Texture/Scatter"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeTileGenerator")
        self.node_operator(layout, "TextureGraphNodeTileSampler")
        self.node_operator(layout, "TextureGraphNodeTileRandom")
        self.node_operator(layout, "TextureGraphNodeTileRandom2")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeSplatter")
        self.node_operator(layout, "TextureGraphNodeSplatterCircular")
        self.node_operator(layout, "TextureGraphNodeArcPavement")
        self.node_operator(layout, "TextureGraphNodeShapeMapper")
        self.node_operator(layout, "TextureGraphNodeScratchesGenerator")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeShapeSplatter")
        self.node_operator(layout, "TextureGraphNodeShapeSplatterBlend")
        self.node_operator(layout, "TextureGraphNodeShapeSplatterDataExtract")
        self.node_operator(layout, "TextureGraphNodeShapeSplatterToMask")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_texture_stone_base(node_add_menu.NodeMenu):
    bl_label = "Stone"
    menu_path = "Texture/Stone"

    def draw(self, _context):
        layout = self.layout
        self.node_operator(layout, "TextureGraphNodeStoneWall")
        self.node_operator(layout, "TextureGraphNodeFittedWall")
        self.node_operator(layout, "TextureGraphNodePackedWall")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_texture_base(node_add_menu.NodeMenu):
    bl_label = "Texture"

    def draw(self, _context):
        layout = self.layout
        self.draw_menu(layout, path="Texture/Noise")
        self.draw_menu(layout, path="Texture/Grunge")
        self.draw_menu(layout, path="Texture/Grunge Maps")
        self.draw_menu(layout, path="Texture/Patterns")
        self.draw_menu(layout, path="Texture/Gradients")
        self.draw_menu(layout, path="Texture/Shapes")
        self.draw_menu(layout, path="Texture/Stone")
        self.draw_menu(layout, path="Texture/Scatter")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeTilingNoise")
        self.node_operator(layout, "TextureGraphNodeFXMap")
        self.node_operator(layout, "TextureGraphNodeText")
        self.node_operator(layout, "TextureGraphNodeSVG")
        layout.separator()
        self.node_operator(layout, "ShaderNodeTexBrick")
        self.node_operator(layout, "ShaderNodeTexChecker")
        self.node_operator(layout, "ShaderNodeTexGabor")
        self.node_operator(layout, "ShaderNodeTexGradient")
        self.node_operator(layout, "ShaderNodeTexMagic")
        self.node_operator(layout, "ShaderNodeTexNoise")
        self.node_operator(layout, "ShaderNodeTexVoronoi")
        self.node_operator(layout, "ShaderNodeTexWave")
        self.node_operator(layout, "ShaderNodeTexWhiteNoise")
        layout.separator()
        self.node_operator(layout, "CompositorNodeBoxMask")
        self.node_operator(layout, "CompositorNodeEllipseMask")

        self.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_texture_graph_node_transform_base(node_add_menu.NodeMenu):
    bl_label = "Transform"

    def draw(self, _context):
        layout = self.layout
        self.draw_menu(layout, path="Transform/Tiling")
        layout.separator()
        self.node_operator(layout, "CompositorNodeTransform")
        self.node_operator(layout, "CompositorNodeTranslate")
        self.node_operator(layout, "CompositorNodeRotate")
        self.node_operator(layout, "CompositorNodeScale")
        self.node_operator(layout, "CompositorNodeFlip")
        layout.separator()
        self.node_operator(layout, "CompositorNodeCrop")
        self.node_operator(layout, "CompositorNodeCornerPin")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeWarp")
        self.node_operator(layout, "TextureGraphNodeDirectionalWarp")
        self.node_operator(layout, "TextureGraphNodeMultiDirectionalWarp")
        self.node_operator(layout, "TextureGraphNodeNonUniformDirectionalWarp")
        self.node_operator(layout, "TextureGraphNodeVectorWarp")
        self.node_operator(layout, "TextureGraphNodeVectorMorph")
        self.node_operator(layout, "TextureGraphNodeSwirl")
        self.node_operator(layout, "CompositorNodeDisplace")
        self.node_operator(layout, "CompositorNodeMapUV")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeSkew")
        self.node_operator(layout, "TextureGraphNodeTrapezoidTransform")
        self.node_operator(layout, "TextureGraphNodeNonSquareTransform")
        self.node_operator(layout, "TextureGraphNodeNonUniformRotation")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeSymmetry")
        self.node_operator(layout, "TextureGraphNodeSymmetrySlice")
        self.node_operator(layout, "TextureGraphNodeCartesianToPolar")
        self.node_operator(layout, "TextureGraphNodePolarToCartesian")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeAutoCrop")
        self.node_operator(layout, "TextureGraphNodeClone")
        layout.separator()
        self.node_operator(layout, "TextureGraphNodeSize")

        self.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_texture_graph_node_utilities_base(node_add_menu.NodeMenu):
    bl_label = "Utilities"

    def draw(self, _context):
        layout = self.layout
        self.draw_menu(layout, path="Utilities/Function")
        self.draw_menu(layout, path="Utilities/Math")
        self.draw_menu(layout, path="Utilities/Vector")
        layout.separator()
        self.node_operator(layout, "CompositorNodeNormalize")
        self.node_operator(layout, "CompositorNodeRelativeToPixel")
        layout.separator()
        self.node_operator(layout, "CompositorNodeSwitch")
        self.node_operator(layout, "GeometryNodeIndexSwitch")
        self.node_operator(layout, "GeometryNodeMenuSwitch")

        self.draw_assets_for_catalog(layout, self.bl_label)


class NODE_MT_texture_graph_node_function_base(node_add_menu.NodeMenu):
    bl_label = "Function"
    menu_path = "Utilities/Function"

    def draw(self, _context):
        layout = self.layout
        # Processors are added as new nodes, there is nothing to swap them in for.
        if getattr(self, "main_operator_id", "") == "node.add_node":
            props = layout.operator("node.texture_graph_add_processor", text="Pixel Processor",
                                    icon='NODE_TG_PIXEL_PROCESSOR')
            props.kind = 'PIXEL'
            props.use_transform = True
            props = layout.operator("node.texture_graph_add_processor", text="Value Processor",
                                    icon='NODE_TG_VALUE_PROCESSOR')
            props.kind = 'VALUE'
            props.use_transform = True
            layout.separator()
        self.node_operator(layout, "TextureGraphNodePosition")
        self.node_operator(layout, "TextureGraphNodeSample")
        self.node_operator(layout, "TextureGraphNodeRandom")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_vector_base(node_add_menu.NodeMenu):
    bl_label = "Vector"
    menu_path = "Utilities/Vector"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "ShaderNodeCombineXYZ")
        self.node_operator(layout, "ShaderNodeSeparateXYZ")
        props = self.node_operator(layout, "ShaderNodeMix", label="Mix Vector")
        ops = props.settings.add()
        ops.name = "data_type"
        ops.value = "'VECTOR'"
        layout.separator()
        self.node_operator(layout, "CompositorNodeNormal")
        self.node_operator(layout, "ShaderNodeRadialTiling")
        self.node_operator(layout, "ShaderNodeVectorCurve")
        self.node_operator_with_searchable_enum(context, layout, "ShaderNodeVectorMath", "operation")
        self.node_operator(layout, "ShaderNodeVectorRotate")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_math_base(node_add_menu.NodeMenu):
    bl_label = "Math"
    menu_path = "Utilities/Math"

    def draw(self, context):
        layout = self.layout
        self.node_operator(layout, "ShaderNodeClamp")
        self.node_operator(layout, "ShaderNodeFloatCurve")
        self.node_operator(layout, "ShaderNodeMapRange")
        self.node_operator_with_searchable_enum(context, layout, "ShaderNodeMath", "operation")
        self.node_operator(layout, "ShaderNodeMix")

        self.draw_assets_for_catalog(layout, self.menu_path)


class NODE_MT_texture_graph_node_all_base(node_add_menu.NodeMenu):
    bl_label = ""
    menu_path = "Root"
    bl_translation_context = i18n_contexts.operator_default

    # NOTE: Menus are looked up via their label, so that the Add and Swap menus can share the
    # same layout.
    def draw(self, context):
        del context
        layout = self.layout
        self.draw_menu(layout, "Input")
        self.draw_menu(layout, "Output")
        layout.separator()
        self.draw_menu(layout, "Texture")
        self.draw_menu(layout, "Color")
        self.draw_menu(layout, "Filter")
        self.draw_menu(layout, "Transform")
        self.draw_menu(layout, "Utilities")
        layout.separator()
        self.draw_root_assets(layout)
        layout.separator()
        self.draw_menu(layout, "Group")
        self.draw_menu(layout, "Layout")


add_menus = {
    # menu bl_idname: baseclass
    "NODE_MT_category_texture_graph_input": NODE_MT_texture_graph_node_input_base,
    "NODE_MT_category_texture_graph_input_constant": NODE_MT_texture_graph_node_input_constant_base,
    "NODE_MT_category_texture_graph_output": NODE_MT_texture_graph_node_output_base,
    "NODE_MT_category_texture_graph_color": NODE_MT_texture_graph_node_color_base,
    "NODE_MT_category_texture_graph_color_adjust": NODE_MT_texture_graph_node_color_adjust_base,
    "NODE_MT_category_texture_graph_color_histogram": NODE_MT_texture_graph_node_color_histogram_base,
    "NODE_MT_category_texture_graph_color_palette": NODE_MT_texture_graph_node_color_palette_base,
    "NODE_MT_category_texture_graph_filter": NODE_MT_texture_graph_node_filter_base,
    "NODE_MT_category_texture_graph_filter_blur": NODE_MT_texture_graph_node_filter_blur_base,
    "NODE_MT_category_texture_graph_filter_effects": NODE_MT_texture_graph_node_filter_effects_base,
    "NODE_MT_category_texture_graph_filter_shape_effects": NODE_MT_texture_graph_node_filter_shape_effects_base,
    "NODE_MT_category_texture_graph_filter_flood_fill": NODE_MT_texture_graph_node_filter_flood_fill_base,
    "NODE_MT_category_texture_graph_filter_normal_map": NODE_MT_texture_graph_node_filter_normal_map_base,
    "NODE_MT_category_texture_graph_transform_tiling": NODE_MT_texture_graph_node_transform_tiling_base,
    "NODE_MT_category_texture_graph_texture": NODE_MT_texture_graph_node_texture_base,
    "NODE_MT_category_texture_graph_texture_noise": NODE_MT_texture_graph_node_texture_noise_base,
    "NODE_MT_category_texture_graph_texture_grunge": NODE_MT_texture_graph_node_texture_grunge_base,
    "NODE_MT_category_texture_graph_texture_grunge_maps": NODE_MT_texture_graph_node_texture_grunge_maps_base,
    "NODE_MT_category_texture_graph_texture_patterns": NODE_MT_texture_graph_node_texture_patterns_base,
    "NODE_MT_category_texture_graph_texture_gradients": NODE_MT_texture_graph_node_texture_gradients_base,
    "NODE_MT_category_texture_graph_texture_shapes": NODE_MT_texture_graph_node_texture_shapes_base,
    "NODE_MT_category_texture_graph_texture_stone": NODE_MT_texture_graph_node_texture_stone_base,
    "NODE_MT_category_texture_graph_texture_scatter": NODE_MT_texture_graph_node_texture_scatter_base,
    "NODE_MT_category_texture_graph_transform": NODE_MT_texture_graph_node_transform_base,
    "NODE_MT_category_texture_graph_utilities": NODE_MT_texture_graph_node_utilities_base,
    "NODE_MT_category_texture_graph_function": NODE_MT_texture_graph_node_function_base,
    "NODE_MT_category_texture_graph_vector": NODE_MT_texture_graph_node_vector_base,
    "NODE_MT_category_texture_graph_math": NODE_MT_texture_graph_node_math_base,
    "NODE_MT_texture_graph_node_add_all": NODE_MT_texture_graph_node_all_base,
}
add_menus = node_add_menu.generate_menus(
    add_menus,
    template=node_add_menu.AddNodeMenu,
    base_dict=node_add_menu.add_base_pathing_dict
)


swap_menus = {
    # menu bl_idname: baseclass
    "NODE_MT_texture_graph_node_input_swap": NODE_MT_texture_graph_node_input_base,
    "NODE_MT_texture_graph_node_input_constant_swap": NODE_MT_texture_graph_node_input_constant_base,
    "NODE_MT_texture_graph_node_output_swap": NODE_MT_texture_graph_node_output_base,
    "NODE_MT_texture_graph_node_color_swap": NODE_MT_texture_graph_node_color_base,
    "NODE_MT_texture_graph_node_color_adjust_swap": NODE_MT_texture_graph_node_color_adjust_base,
    "NODE_MT_texture_graph_node_color_histogram_swap": NODE_MT_texture_graph_node_color_histogram_base,
    "NODE_MT_texture_graph_node_color_palette_swap": NODE_MT_texture_graph_node_color_palette_base,
    "NODE_MT_texture_graph_node_filter_swap": NODE_MT_texture_graph_node_filter_base,
    "NODE_MT_texture_graph_node_filter_blur_swap": NODE_MT_texture_graph_node_filter_blur_base,
    "NODE_MT_texture_graph_node_filter_effects_swap": NODE_MT_texture_graph_node_filter_effects_base,
    "NODE_MT_texture_graph_node_filter_shape_effects_swap": NODE_MT_texture_graph_node_filter_shape_effects_base,
    "NODE_MT_texture_graph_node_filter_flood_fill_swap": NODE_MT_texture_graph_node_filter_flood_fill_base,
    "NODE_MT_texture_graph_node_filter_normal_map_swap": NODE_MT_texture_graph_node_filter_normal_map_base,
    "NODE_MT_texture_graph_node_transform_tiling_swap": NODE_MT_texture_graph_node_transform_tiling_base,
    "NODE_MT_texture_graph_node_texture_swap": NODE_MT_texture_graph_node_texture_base,
    "NODE_MT_texture_graph_node_texture_noise_swap": NODE_MT_texture_graph_node_texture_noise_base,
    "NODE_MT_texture_graph_node_texture_grunge_swap": NODE_MT_texture_graph_node_texture_grunge_base,
    "NODE_MT_texture_graph_node_texture_grunge_maps_swap": NODE_MT_texture_graph_node_texture_grunge_maps_base,
    "NODE_MT_texture_graph_node_texture_patterns_swap": NODE_MT_texture_graph_node_texture_patterns_base,
    "NODE_MT_texture_graph_node_texture_gradients_swap": NODE_MT_texture_graph_node_texture_gradients_base,
    "NODE_MT_texture_graph_node_texture_shapes_swap": NODE_MT_texture_graph_node_texture_shapes_base,
    "NODE_MT_texture_graph_node_texture_stone_swap": NODE_MT_texture_graph_node_texture_stone_base,
    "NODE_MT_texture_graph_node_texture_scatter_swap": NODE_MT_texture_graph_node_texture_scatter_base,
    "NODE_MT_texture_graph_node_transform_swap": NODE_MT_texture_graph_node_transform_base,
    "NODE_MT_texture_graph_node_utilities_swap": NODE_MT_texture_graph_node_utilities_base,
    "NODE_MT_texture_graph_node_function_swap": NODE_MT_texture_graph_node_function_base,
    "NODE_MT_texture_graph_node_vector_swap": NODE_MT_texture_graph_node_vector_base,
    "NODE_MT_texture_graph_node_math_swap": NODE_MT_texture_graph_node_math_base,
    "NODE_MT_texture_graph_node_swap_all": NODE_MT_texture_graph_node_all_base,
}
swap_menus = node_add_menu.generate_menus(
    swap_menus,
    template=node_add_menu.SwapNodeMenu,
    base_dict=node_add_menu.swap_base_pathing_dict
)


classes = (
    *add_menus,
    *swap_menus,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)

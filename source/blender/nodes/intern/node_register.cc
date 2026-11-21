/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_composite.hh"
#include "NOD_geometry.hh"
#include "NOD_register.hh"
#include "NOD_socket.hh"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"

#include "BLT_translation.hh"

#include "RNA_define.hh"

#include "UI_resources.hh"

namespace blender {

static bool node_undefined_poll(const bke::bNodeType * /*ntype*/,
                                const bNodeTree * /*nodetree*/,
                                const char ** /*r_disabled_hint*/)
{
  /* this type can not be added deliberately, it's just a placeholder */
  return false;
}

/* register fallback types used for undefined tree, nodes, sockets */
static void register_undefined_types()
{
  /* NOTE: these types are not registered in the type hashes,
   * they are just used as placeholders in case the actual types are not registered.
   */

  bke::NodeTreeTypeUndefined.type = NTREE_UNDEFINED;
  bke::NodeTreeTypeUndefined.idname = "NodeTreeUndefined";
  bke::NodeTreeTypeUndefined.ui_name = N_("Undefined");
  bke::NodeTreeTypeUndefined.ui_description = N_("Undefined Node Tree Type");

  bke::node_type_base_custom(bke::NodeTypeUndefined, "NodeUndefined", "Undefined", "UNDEFINED", 0);
  bke::NodeTypeUndefined.poll = node_undefined_poll;

  bke::NodeSocketTypeUndefined.idname = "NodeSocketUndefined";
  /* extra type info for standard socket types */
  bke::NodeSocketTypeUndefined.type = SOCK_CUSTOM;
  bke::NodeSocketTypeUndefined.subtype = PROP_NONE;

  bke::NodeSocketTypeUndefined.use_link_limits_of_type = true;
  bke::NodeSocketTypeUndefined.input_link_limit = 0xFFF;
  bke::NodeSocketTypeUndefined.output_link_limit = 0xFFF;
}

class SimulationZoneType : public bke::bNodeZoneType {
 public:
  SimulationZoneType()
  {
    this->input_idname = "GeometryNodeSimulationInput";
    this->output_idname = "GeometryNodeSimulationOutput";
    this->input_type = GEO_NODE_SIMULATION_INPUT;
    this->output_type = GEO_NODE_SIMULATION_OUTPUT;
    this->theme_id = TH_NODE_ZONE_SIMULATION;
  }

  const int &get_corresponding_output_id(const bNode &input_bnode) const override
  {
    BLI_assert(input_bnode.type_legacy == this->input_type);
    return static_cast<NodeGeometrySimulationInput *>(input_bnode.storage)->output_node_id;
  }
};

class RepeatZoneType : public bke::bNodeZoneType {
 public:
  RepeatZoneType()
  {
    this->input_idname = "GeometryNodeRepeatInput";
    this->output_idname = "GeometryNodeRepeatOutput";
    this->input_type = GEO_NODE_REPEAT_INPUT;
    this->output_type = GEO_NODE_REPEAT_OUTPUT;
    this->theme_id = TH_NODE_ZONE_REPEAT;
  }

  const int &get_corresponding_output_id(const bNode &input_bnode) const override
  {
    BLI_assert(input_bnode.type_legacy == this->input_type);
    return static_cast<NodeGeometryRepeatInput *>(input_bnode.storage)->output_node_id;
  }
};

class ForeachGeometryElementZoneType : public bke::bNodeZoneType {
 public:
  ForeachGeometryElementZoneType()
  {
    this->input_idname = "GeometryNodeForeachGeometryElementInput";
    this->output_idname = "GeometryNodeForeachGeometryElementOutput";
    this->input_type = GEO_NODE_FOREACH_GEOMETRY_ELEMENT_INPUT;
    this->output_type = GEO_NODE_FOREACH_GEOMETRY_ELEMENT_OUTPUT;
    this->theme_id = TH_NODE_ZONE_FOREACH_GEOMETRY_ELEMENT;
  }

  const int &get_corresponding_output_id(const bNode &input_bnode) const override
  {
    BLI_assert(input_bnode.type_legacy == this->input_type);
    return static_cast<NodeGeometryForeachGeometryElementInput *>(input_bnode.storage)
        ->output_node_id;
  }
};

class ClosureZoneType : public bke::bNodeZoneType {
 public:
  ClosureZoneType()
  {
    this->input_idname = "NodeClosureInput";
    this->output_idname = "NodeClosureOutput";
    this->input_type = NODE_CLOSURE_INPUT;
    this->output_type = NODE_CLOSURE_OUTPUT;
    this->theme_id = TH_NODE_ZONE_CLOSURE;
  }

  const int &get_corresponding_output_id(const bNode &input_bnode) const override
  {
    BLI_assert(input_bnode.type_legacy == this->input_type);
    return static_cast<NodeClosureInput *>(input_bnode.storage)->output_node_id;
  }
};

/* Show geometry nodes that do a modeling operation with that operation's icon, recolored, and the
 * others with an icon drawn for them (`GEO_N_*`). */
static void set_geometry_node_icons()
{
  const struct {
    const char *idname;
    int icon;
  } node_icons[] = {
      {"FunctionNodeAlignEulerToVector", ICON_GEO_N_ALIGN_EULER_TO_VECTOR},
      {"FunctionNodeAlignRotationToVector", ICON_GEO_N_ALIGN_ROTATION_TO_VECTOR},
      {"FunctionNodeAxesToRotation", ICON_GEO_N_AXES_TO_ROTATION},
      {"FunctionNodeAxisAngleToRotation", ICON_GEO_N_AXIS_ANGLE_TO_ROTATION},
      {"FunctionNodeBitMath", ICON_GEO_N_BIT_MATH},
      {"FunctionNodeBooleanMath", ICON_GEO_N_BOOLEAN_MATH},
      {"FunctionNodeCombineColor", ICON_GEO_N_COMBINE_COLOR},
      {"FunctionNodeCombineMatrix", ICON_GEO_N_COMBINE_MATRIX},
      {"FunctionNodeCombineTransform", ICON_GEO_N_COMBINE_TRANSFORM},
      {"FunctionNodeCompare", ICON_GEO_N_COMPARE},
      {"FunctionNodeEulerToRotation", ICON_GEO_N_EULER_TO_ROTATION},
      {"FunctionNodeFindInString", ICON_GEO_N_FIND_IN_STRING},
      {"FunctionNodeFloatToInt", ICON_GEO_N_FLOAT_TO_INT},
      {"FunctionNodeFormatString", ICON_GEO_N_FORMAT_STRING},
      {"FunctionNodeHashValue", ICON_GEO_N_HASH_VALUE},
      {"FunctionNodeInputBool", ICON_GEO_N_INPUT_BOOL},
      {"FunctionNodeInputColor", ICON_GEO_N_INPUT_COLOR},
      {"FunctionNodeInputInt", ICON_GEO_N_INPUT_INT},
      {"FunctionNodeInputRotation", ICON_GEO_N_INPUT_ROTATION},
      {"FunctionNodeInputSpecialCharacters", ICON_GEO_N_INPUT_SPECIAL_CHARACTERS},
      {"FunctionNodeInputString", ICON_GEO_N_INPUT_STRING},
      {"FunctionNodeInputVector", ICON_GEO_N_INPUT_VECTOR},
      {"FunctionNodeIntegerMath", ICON_GEO_N_INTEGER_MATH},
      {"FunctionNodeInvertMatrix", ICON_GEO_N_INVERT_MATRIX},
      {"FunctionNodeInvertRotation", ICON_GEO_N_INVERT_ROTATION},
      {"FunctionNodeMatchString", ICON_GEO_N_MATCH_STRING},
      {"FunctionNodeMatrixDeterminant", ICON_GEO_N_MATRIX_DETERMINANT},
      {"FunctionNodeMatrixMultiply", ICON_GEO_N_MATRIX_MULTIPLY},
      {"FunctionNodeMatrixSVD", ICON_GEO_N_MATRIX_SVD},
      {"FunctionNodeProjectPoint", ICON_GEO_N_PROJECT_POINT},
      {"FunctionNodeQuaternionToRotation", ICON_GEO_N_QUATERNION_TO_ROTATION},
      {"FunctionNodeRandomValue", ICON_GEO_N_RANDOM_VALUE},
      {"FunctionNodeReplaceString", ICON_GEO_N_REPLACE_STRING},
      {"FunctionNodeRotateEuler", ICON_GEO_N_ROTATE_EULER},
      {"FunctionNodeRotateRotation", ICON_GEO_N_ROTATE_ROTATION},
      {"FunctionNodeRotateVector", ICON_GEO_N_ROTATE_VECTOR},
      {"FunctionNodeRotationToAxisAngle", ICON_GEO_N_ROTATION_TO_AXIS_ANGLE},
      {"FunctionNodeRotationToEuler", ICON_GEO_N_ROTATION_TO_EULER},
      {"FunctionNodeRotationToQuaternion", ICON_GEO_N_ROTATION_TO_QUATERNION},
      {"FunctionNodeSeparateColor", ICON_GEO_N_SEPARATE_COLOR},
      {"FunctionNodeSeparateMatrix", ICON_GEO_N_SEPARATE_MATRIX},
      {"FunctionNodeSeparateTransform", ICON_GEO_N_SEPARATE_TRANSFORM},
      {"FunctionNodeSliceString", ICON_GEO_N_SLICE_STRING},
      {"FunctionNodeStringLength", ICON_GEO_N_STRING_LENGTH},
      {"FunctionNodeStringToValue", ICON_GEO_N_STRING_TO_VALUE},
      {"FunctionNodeTransformDirection", ICON_GEO_N_TRANSFORM_DIRECTION},
      {"FunctionNodeTransformPoint", ICON_GEO_N_TRANSFORM_POINT},
      {"FunctionNodeTransposeMatrix", ICON_GEO_N_TRANSPOSE_MATRIX},
      {"FunctionNodeValueToString", ICON_GEO_N_VALUE_TO_STRING},
      {"GeometryNodeAccumulateField", ICON_GEO_N_ACCUMULATE_FIELD},
      {"GeometryNodeAttributeDomainSize", ICON_GEO_N_ATTRIBUTE_DOMAIN_SIZE},
      {"GeometryNodeAttributeStatistic", ICON_GEO_N_ATTRIBUTE_STATISTIC},
      {"GeometryNodeBake", ICON_GEO_N_BAKE},
      {"GeometryNodeBevelMesh", ICON_GEO_MOD_BEVEL},
      {"GeometryNodeBlurAttribute", ICON_GEO_N_BLUR_ATTRIBUTE},
      {"GeometryNodeBoneInfo", ICON_GEO_N_BONE_INFO},
      {"GeometryNodeBoundBox", ICON_GEO_N_BOUND_BOX},
      {"GeometryNodeCameraInfo", ICON_GEO_N_CAMERA_INFO},
      {"GeometryNodeCaptureAttribute", ICON_GEO_N_CAPTURE_ATTRIBUTE},
      {"GeometryNodeClipMesh", ICON_GEO_BISECT},
      {"GeometryNodeCollectionInfo", ICON_GEO_OUTLINER_COLLECTION},
      {"GeometryNodeConvexHull", ICON_GEO_N_CONVEX_HULL},
      {"GeometryNodeCornersOfEdge", ICON_GEO_N_CORNERS_OF_EDGE},
      {"GeometryNodeCornersOfFace", ICON_GEO_N_CORNERS_OF_FACE},
      {"GeometryNodeCornersOfVertex", ICON_GEO_N_CORNERS_OF_VERTEX},
      {"GeometryNodeCubeGridTopology", ICON_GEO_N_CUBE_GRID_TOPOLOGY},
      {"GeometryNodeCurveArc", ICON_GEO_N_CURVE_ARC},
      {"GeometryNodeCurveEndpointSelection", ICON_GEO_N_CURVE_ENDPOINT_SELECTION},
      {"GeometryNodeCurveHandleTypeSelection", ICON_GEO_N_CURVE_HANDLE_TYPE_SELECTION},
      {"GeometryNodeCurveLength", ICON_GEO_N_CURVE_LENGTH},
      {"GeometryNodeCurveOfPoint", ICON_GEO_N_CURVE_OF_POINT},
      {"GeometryNodeCurvePrimitiveBezierSegment", ICON_GEO_CURVE_BEZCURVE},
      {"GeometryNodeCurvePrimitiveCircle", ICON_GEO_CURVE_NCIRCLE},
      {"GeometryNodeCurvePrimitiveLine", ICON_GEO_N_CURVE_PRIMITIVE_LINE},
      {"GeometryNodeCurvePrimitiveQuadrilateral", ICON_GEO_N_CURVE_PRIMITIVE_QUADRILATERAL},
      {"GeometryNodeCurveQuadraticBezier", ICON_GEO_N_CURVE_QUADRATIC_BEZIER},
      {"GeometryNodeCurveSetHandles", ICON_GEO_N_CURVE_SET_HANDLES},
      {"GeometryNodeCurveSpiral", ICON_GEO_N_CURVE_SPIRAL},
      {"GeometryNodeCurveSplineType", ICON_GEO_N_CURVE_SPLINE_TYPE},
      {"GeometryNodeCurveStar", ICON_GEO_N_CURVE_STAR},
      {"GeometryNodeCurveToMesh", ICON_GEO_N_CURVE_TO_MESH},
      {"GeometryNodeCurveToPoints", ICON_GEO_N_CURVE_TO_POINTS},
      {"GeometryNodeCurvesToGreasePencil", ICON_GEO_N_CURVES_TO_GREASE_PENCIL},
      {"GeometryNodeDecimateMesh", ICON_GEO_N_DECIMATE_MESH},
      {"GeometryNodeDeformCurvesOnSurface", ICON_GEO_N_DEFORM_CURVES_ON_SURFACE},
      {"GeometryNodeDeleteGeometry", ICON_GEO_TRASH},
      {"GeometryNodeDiceMesh", ICON_GEO_MOD_DICE},
      {"GeometryNodeDivideMesh", ICON_GEO_MOD_DIVIDE},
      {"GeometryNodeCollapseIslands", ICON_GEO_MOD_COLLAPSE},
      {"GeometryNodeDrapeMesh", ICON_GEO_MOD_DRAPE},
      {"GeometryNodeShatterMesh", ICON_GEO_MOD_SHATTER},
      {"GeometryNodeMasonryMesh", ICON_GEO_MOD_MASONRY},
      {"GeometryNodePlanks", ICON_GEO_MOD_PLANKS},
      {"GeometryNodeRoof", ICON_GEO_MOD_ROOF},
      {"GeometryNodeCarve", ICON_GEO_MOD_CARVE},
      {"GeometryNodeCleanupMesh", ICON_GEO_MOD_CLEANUP},
      {"GeometryNodeStonework", ICON_GEO_MOD_STONEWORK},
      {"GeometryNodeDistribute", ICON_GEO_MOD_DISTRIBUTE},
      {"GeometryNodeMaterialize", ICON_GEO_MOD_MATERIALIZE},
      {"GeometryNodeDisplaceMesh", ICON_GEO_MOD_DISPLACE},
      {"GeometryNodeDissolveMesh", ICON_GEO_MOD_DISSOLVE},
      {"GeometryNodeDistributePointsInGrid", ICON_GEO_N_DISTRIBUTE_POINTS_IN_GRID},
      {"GeometryNodeDistributePointsInVolume", ICON_GEO_N_DISTRIBUTE_POINTS_IN_VOLUME},
      {"GeometryNodeDistributePointsOnFaces", ICON_GEO_N_DISTRIBUTE_POINTS_ON_FACES},
      {"GeometryNodeDualMesh", ICON_GEO_N_DUAL_MESH},
      {"GeometryNodeDuplicateElements", ICON_GEO_DUPLICATE},
      {"GeometryNodeEdgePathsToCurves", ICON_GEO_N_EDGE_PATHS_TO_CURVES},
      {"GeometryNodeEdgePathsToSelection", ICON_GEO_N_EDGE_PATHS_TO_SELECTION},
      {"GeometryNodeEdgesOfCorner", ICON_GEO_N_EDGES_OF_CORNER},
      {"GeometryNodeEdgesOfVertex", ICON_GEO_N_EDGES_OF_VERTEX},
      {"GeometryNodeEdgesToFaceGroups", ICON_GEO_N_EDGES_TO_FACE_GROUPS},
      {"GeometryNodeExtrudeMesh", ICON_GEO_EXTRUDE},
      {"GeometryNodeFaceOfCorner", ICON_GEO_N_FACE_OF_CORNER},
      {"GeometryNodeFieldAtIndex", ICON_GEO_N_FIELD_AT_INDEX},
      {"GeometryNodeFieldAverage", ICON_GEO_N_FIELD_AVERAGE},
      {"GeometryNodeFieldMinAndMax", ICON_GEO_N_FIELD_MIN_AND_MAX},
      {"GeometryNodeFieldOnDomain", ICON_GEO_N_FIELD_ON_DOMAIN},
      {"GeometryNodeFieldToGrid", ICON_GEO_N_FIELD_TO_GRID},
      {"GeometryNodeFieldToList", ICON_GEO_N_FIELD_TO_LIST},
      {"GeometryNodeFieldVariance", ICON_GEO_N_FIELD_VARIANCE},
      {"GeometryNodeFillCurve", ICON_GEO_N_FILL_CURVE},
      {"GeometryNodeFilletCurve", ICON_GEO_N_FILLET_CURVE},
      {"GeometryNodeFlipFaces", ICON_GEO_NORMALS_FACE},
      {"GeometryNodeForeachGeometryElementInput", ICON_GEO_N_FOREACH_GEOMETRY_ELEMENT_INPUT},
      {"GeometryNodeForeachGeometryElementOutput", ICON_GEO_N_FOREACH_GEOMETRY_ELEMENT_OUTPUT},
      {"GeometryNodeGeometryToInstance", ICON_GEO_N_GEOMETRY_TO_INSTANCE},
      {"GeometryNodeGetGeometryBundle", ICON_GEO_N_GET_GEOMETRY_BUNDLE},
      {"GeometryNodeGetNamedGrid", ICON_GEO_N_GET_NAMED_GRID},
      {"GeometryNodeGizmoDial", ICON_GEO_N_GIZMO_DIAL},
      {"GeometryNodeGizmoLinear", ICON_GEO_N_GIZMO_LINEAR},
      {"GeometryNodeGizmoTransform", ICON_GEO_N_GIZMO_TRANSFORM},
      {"GeometryNodeGreasePencilToCurves", ICON_GEO_N_GREASE_PENCIL_TO_CURVES},
      {"GeometryNodeGridAdvect", ICON_GEO_N_GRID_ADVECT},
      {"GeometryNodeGridClip", ICON_GEO_N_GRID_CLIP},
      {"GeometryNodeGridCurl", ICON_GEO_N_GRID_CURL},
      {"GeometryNodeGridDilateAndErode", ICON_GEO_N_GRID_DILATE_AND_ERODE},
      {"GeometryNodeGridDivergence", ICON_GEO_N_GRID_DIVERGENCE},
      {"GeometryNodeGridGradient", ICON_GEO_N_GRID_GRADIENT},
      {"GeometryNodeGridInfo", ICON_GEO_N_GRID_INFO},
      {"GeometryNodeGridLaplacian", ICON_GEO_N_GRID_LAPLACIAN},
      {"GeometryNodeGridMean", ICON_GEO_N_GRID_MEAN},
      {"GeometryNodeGridMedian", ICON_GEO_N_GRID_MEDIAN},
      {"GeometryNodeGridPrune", ICON_GEO_N_GRID_PRUNE},
      {"GeometryNodeGridToMesh", ICON_GEO_N_GRID_TO_MESH},
      {"GeometryNodeGridToPoints", ICON_GEO_N_GRID_TO_POINTS},
      {"GeometryNodeGridVoxelize", ICON_GEO_N_GRID_VOXELIZE},
      {"GeometryNodeGroup", ICON_GEO_N_GROUP},
      {"GeometryNodeImageInfo", ICON_GEO_N_IMAGE_INFO},
      {"GeometryNodeImageTexture", ICON_GEO_N_IMAGE_TEXTURE},
      {"GeometryNodeImportCSV", ICON_GEO_N_IMPORT_CSV},
      {"GeometryNodeImportOBJ", ICON_GEO_N_IMPORT_OBJ},
      {"GeometryNodeImportPLY", ICON_GEO_N_IMPORT_PLY},
      {"GeometryNodeImportSTL", ICON_GEO_N_IMPORT_STL},
      {"GeometryNodeImportText", ICON_GEO_N_IMPORT_TEXT},
      {"GeometryNodeImportVDB", ICON_GEO_N_IMPORT_VDB},
      {"GeometryNodeIndexOfNearest", ICON_GEO_N_INDEX_OF_NEAREST},
      {"GeometryNodeIndexSwitch", ICON_GEO_N_INDEX_SWITCH},
      {"GeometryNodeInputActiveCamera", ICON_GEO_N_INPUT_ACTIVE_CAMERA},
      {"GeometryNodeInputCollection", ICON_GEO_N_INPUT_COLLECTION},
      {"GeometryNodeInputCurveHandlePositions", ICON_GEO_N_INPUT_CURVE_HANDLE_POSITIONS},
      {"GeometryNodeInputCurveTilt", ICON_GEO_N_INPUT_CURVE_TILT},
      {"GeometryNodeInputEdgeSmooth", ICON_GEO_N_INPUT_EDGE_SMOOTH},
      {"GeometryNodeInputID", ICON_GEO_N_INPUT_ID},
      {"GeometryNodeInputImage", ICON_GEO_N_INPUT_IMAGE},
      {"GeometryNodeInputIndex", ICON_GEO_N_INPUT_INDEX},
      {"GeometryNodeInputInstanceBounds", ICON_GEO_N_INPUT_INSTANCE_BOUNDS},
      {"GeometryNodeInputInstanceRotation", ICON_GEO_N_INPUT_INSTANCE_ROTATION},
      {"GeometryNodeInputInstanceScale", ICON_GEO_N_INPUT_INSTANCE_SCALE},
      {"GeometryNodeInputMaterial", ICON_GEO_N_INPUT_MATERIAL},
      {"GeometryNodeInputMaterialIndex", ICON_GEO_N_INPUT_MATERIAL_INDEX},
      {"GeometryNodeInputMeshEdgeAngle", ICON_GEO_N_INPUT_MESH_EDGE_ANGLE},
      {"GeometryNodeInputMeshEdgeNeighbors", ICON_GEO_N_INPUT_MESH_EDGE_NEIGHBORS},
      {"GeometryNodeInputMeshEdgeVertices", ICON_GEO_N_INPUT_MESH_EDGE_VERTICES},
      {"GeometryNodeInputMeshFaceArea", ICON_GEO_N_INPUT_MESH_FACE_AREA},
      {"GeometryNodeInputMeshFaceIsPlanar", ICON_GEO_N_INPUT_MESH_FACE_IS_PLANAR},
      {"GeometryNodeInputMeshFaceNeighbors", ICON_GEO_N_INPUT_MESH_FACE_NEIGHBORS},
      {"GeometryNodeInputMeshIsland", ICON_GEO_N_INPUT_MESH_ISLAND},
      {"GeometryNodeInputMeshVertexNeighbors", ICON_GEO_N_INPUT_MESH_VERTEX_NEIGHBORS},
      {"GeometryNodeInputNamedAttribute", ICON_GEO_N_INPUT_NAMED_ATTRIBUTE},
      {"GeometryNodeInputNamedLayerSelection", ICON_GEO_N_INPUT_NAMED_LAYER_SELECTION},
      {"GeometryNodeInputNormal", ICON_GEO_N_INPUT_NORMAL},
      {"GeometryNodeInputObject", ICON_GEO_N_INPUT_OBJECT},
      {"GeometryNodeInputPosition", ICON_GEO_N_INPUT_POSITION},
      {"GeometryNodeInputRadius", ICON_GEO_N_INPUT_RADIUS},
      {"GeometryNodeInputSceneTime", ICON_GEO_N_INPUT_SCENE_TIME},
      {"GeometryNodeInputShadeSmooth", ICON_GEO_N_INPUT_SHADE_SMOOTH},
      {"GeometryNodeInputShortestEdgePaths", ICON_GEO_N_INPUT_SHORTEST_EDGE_PATHS},
      {"GeometryNodeInputSplineCyclic", ICON_GEO_N_INPUT_SPLINE_CYCLIC},
      {"GeometryNodeInputSplineResolution", ICON_GEO_N_INPUT_SPLINE_RESOLUTION},
      {"GeometryNodeInputTangent", ICON_GEO_N_INPUT_TANGENT},
      {"GeometryNodeInputVoxelIndex", ICON_GEO_N_INPUT_VOXEL_INDEX},
      {"GeometryNodeInstanceOnPoints", ICON_GEO_MOD_PARTICLE_INSTANCE},
      {"GeometryNodeInstanceTransform", ICON_GEO_N_INSTANCE_TRANSFORM},
      {"GeometryNodeInstancesToPoints", ICON_GEO_N_INSTANCES_TO_POINTS},
      {"GeometryNodeInterpolateCurves", ICON_GEO_N_INTERPOLATE_CURVES},
      {"GeometryNodeIsViewport", ICON_GEO_N_IS_VIEWPORT},
      {"GeometryNodeIsotropicRemesh", ICON_GEO_MOD_ISOTROPIC_REMESH},
      {"GeometryNodeSolidify", ICON_GEO_MOD_SOLIDIFY},
      {"GeometryNodeWireframe", ICON_GEO_MOD_WIREFRAME},
      {"GeometryNodeScrew", ICON_GEO_MOD_SCREW},
      {"GeometryNodeMirror", ICON_GEO_MOD_MIRROR},
      {"GeometryNodeSkin", ICON_GEO_MOD_SKIN},
      {"GeometryNodeCast", ICON_GEO_MOD_CAST},
      {"GeometryNodeWave", ICON_GEO_MOD_WAVE},
      {"GeometryNodeWarp", ICON_GEO_MOD_WARP},
      {"GeometryNodeWeightedNormal", ICON_GEO_MOD_WEIGHTED_NORMAL},
      {"GeometryNodeLaplacianSmooth", ICON_GEO_MOD_LAPLACIAN_SMOOTH},
      {"GeometryNodeShrinkwrap", ICON_GEO_MOD_SHRINKWRAP},
      {"GeometryNodeSmoothModifier", ICON_GEO_MOD_SMOOTH},
      {"GeometryNodeSimpleDeform", ICON_GEO_MOD_SIMPLEDEFORM},
      {"GeometryNodeArrayModifier", ICON_GEO_MOD_ARRAY},
      {"GeometryNodeEdgeSplitModifier", ICON_GEO_MOD_EDGESPLIT},
      {"GeometryNodeUVWarp", ICON_GEO_MOD_UVPROJECT},
      {"GeometryNodeNormalEdit", ICON_GEO_MOD_NORMALEDIT},
      {"GeometryNodeLatticeDeform", ICON_GEO_MOD_LATTICE},
      {"GeometryNodeCurveDeform", ICON_GEO_MOD_CURVE},
      {"GeometryNodeBuild", ICON_GEO_MOD_BUILD},
      {"GeometryNodeRemeshModifier", ICON_GEO_MOD_REMESH},
      {"GeometryNodeCapHoles", ICON_GEO_MOD_CAP_HOLES},
      {"GeometryNodeUVWMap", ICON_GEO_MOD_UVW_MAP},
      {"GeometryNodeUVScale", ICON_GEO_MOD_UV_SCALE},
      {"GeometryNodeMeshTension", ICON_GEO_MOD_TENSION},
      {"GeometryNodeRadialArray", ICON_GEO_MOD_RADIAL_ARRAY},
      {"GeometryNodeVolumeSelect", ICON_GEO_MOD_VOL_SELECT},
      {"GeometryNodeMaterialByElement", ICON_GEO_MOD_MATERIAL_BY_ELEMENT},
      {"GeometryNodeDataChannel", ICON_GEO_MOD_DATA_CHANNEL},
      {"GeometryNodeFaceExtrude", ICON_GEO_MOD_FACE_EXTRUDE},
      {"GeometryNodeQuadrangulate", ICON_GEO_MOD_QUADRANGULATE},
      {"GeometryNodeRandomizeElements", ICON_GEO_MOD_RANDOMIZE_ELEMENTS},
      {"GeometryNodeExplodedView", ICON_GEO_MOD_EXPLODED_VIEW},
      {"GeometryNodeElementOrder", ICON_GEO_MOD_ELEMENT_ORDER},
      {"GeometryNodeElementPivot", ICON_GEO_MOD_ELEMENT_PIVOT},
      {"GeometryNodeMarkByAngle", ICON_GEO_MOD_MARK_BY_ANGLE},
      {"GeometryNodeNoiseDeform", ICON_GEO_MOD_NOISE_DEFORM},
      {"GeometryNodeFFD", ICON_GEO_MOD_FFD_3},
      {"GeometryNodePanelize", ICON_GEO_MOD_PANELIZE},
      {"GeometryNodeInset", ICON_GEO_MOD_INSET},
      {"GeometryNodePoke", ICON_GEO_MOD_POKE},
      {"GeometryNodeBeautifyFaces", ICON_GEO_MOD_BEAUTIFY},
      {"GeometryNodeRotateEdges", ICON_GEO_MOD_ROTATE_EDGES},
      {"GeometryNodeSymmetrize", ICON_GEO_MOD_SYMMETRIZE},
      {"GeometryNodePush", ICON_GEO_MOD_PUSH},
      {"GeometryNodeMelt", ICON_GEO_MOD_MELT},
      {"GeometryNodeSmartUVProject", ICON_GEO_MOD_SMART_UV_PROJECT},
      {"GeometryNodeBend", ICON_GEO_MOD_BEND},
      {"GeometryNodeTwist", ICON_GEO_MOD_TWIST},
      {"GeometryNodeTaper", ICON_GEO_MOD_TAPER},
      {"GeometryNodeStretch", ICON_GEO_MOD_STRETCH},
      {"GeometryNodeMorphCurves", ICON_GEO_MOD_SPLINE_MORPH},
      {"GeometryNodeRelaxCurves", ICON_GEO_MOD_RELAX_SPLINE},
      {"GeometryNodeJoinGeometry", ICON_GEO_N_JOIN_GEOMETRY},
      {"GeometryNodeListGetItem", ICON_GEO_N_LIST_GET_ITEM},
      {"GeometryNodeListLength", ICON_GEO_N_LIST_LENGTH},
      {"GeometryNodeMaterialSelection", ICON_GEO_N_MATERIAL_SELECTION},
      {"GeometryNodeMenuSwitch", ICON_GEO_N_MENU_SWITCH},
      {"GeometryNodeMergeByDistance", ICON_GEO_AUTOMERGE_OFF},
      {"GeometryNodeMergeLayers", ICON_GEO_N_MERGE_LAYERS},
      {"GeometryNodeMeshBoolean", ICON_GEO_MOD_BOOLEAN},
      {"GeometryNodeMeshCircle", ICON_GEO_MESH_CIRCLE},
      {"GeometryNodeMeshCone", ICON_GEO_MESH_CONE},
      {"GeometryNodeMeshCube", ICON_GEO_MESH_CUBE},
      {"GeometryNodeMeshCylinder", ICON_GEO_MESH_CYLINDER},
      {"GeometryNodeMeshFaceSetBoundaries", ICON_GEO_N_MESH_FACE_SET_BOUNDARIES},
      {"GeometryNodeMeshGrid", ICON_GEO_MESH_GRID},
      {"GeometryNodeMeshIcoSphere", ICON_GEO_MESH_ICOSPHERE},
      {"GeometryNodeMeshLine", ICON_GEO_N_MESH_LINE},
      {"GeometryNodeMeshToCurve", ICON_GEO_OUTLINER_OB_CURVE},
      {"GeometryNodeMeshToDensityGrid", ICON_GEO_N_MESH_TO_DENSITY_GRID},
      {"GeometryNodeMeshToPoints", ICON_GEO_N_MESH_TO_POINTS},
      {"GeometryNodeMeshToSDFGrid", ICON_GEO_N_MESH_TO_SDF_GRID},
      {"GeometryNodeMeshToVolume", ICON_GEO_N_MESH_TO_VOLUME},
      {"GeometryNodeMeshUVSphere", ICON_GEO_MESH_UVSPHERE},
      {"GeometryNodeObjectInfo", ICON_GEO_OBJECT_DATA},
      {"GeometryNodeOffsetCornerInFace", ICON_GEO_N_OFFSET_CORNER_IN_FACE},
      {"GeometryNodeOffsetPointInCurve", ICON_GEO_N_OFFSET_POINT_IN_CURVE},
      {"GeometryNodePoints", ICON_GEO_N_POINTS},
      {"GeometryNodePointsOfCurve", ICON_GEO_N_POINTS_OF_CURVE},
      {"GeometryNodePointsToCurves", ICON_GEO_OUTLINER_OB_CURVES},
      {"GeometryNodePointsToSDFGrid", ICON_GEO_N_POINTS_TO_SDF_GRID},
      {"GeometryNodePointsToVertices", ICON_GEO_N_POINTS_TO_VERTICES},
      {"GeometryNodePointsToVolume", ICON_GEO_N_POINTS_TO_VOLUME},
      {"GeometryNodeProximity", ICON_GEO_N_PROXIMITY},
      {"GeometryNodeQuadriFlowRemesh", ICON_GEO_N_QUADRI_FLOW_REMESH},
      {"GeometryNodeRaycast", ICON_GEO_N_RAYCAST},
      {"GeometryNodeRealizeInstances", ICON_GEO_N_REALIZE_INSTANCES},
      {"GeometryNodeRelaxMesh", ICON_GEO_MOD_RELAX},
      {"GeometryNodeRemoveAttribute", ICON_GEO_N_REMOVE_ATTRIBUTE},
      {"GeometryNodeRepeatInput", ICON_GEO_N_REPEAT_INPUT},
      {"GeometryNodeRepeatOutput", ICON_GEO_N_REPEAT_OUTPUT},
      {"GeometryNodeReplaceMaterial", ICON_GEO_N_REPLACE_MATERIAL},
      {"GeometryNodeResampleCurve", ICON_GEO_N_RESAMPLE_CURVE},
      {"GeometryNodeResampleMesh", ICON_GEO_RESAMPLE},
      {"GeometryNodeReverseCurve", ICON_GEO_N_REVERSE_CURVE},
      {"GeometryNodeRotateInstances", ICON_GEO_N_ROTATE_INSTANCES},
      {"GeometryNodeSDFGridBoolean", ICON_GEO_N_SDF_GRID_BOOLEAN},
      {"GeometryNodeSDFGridFillet", ICON_GEO_N_SDF_GRID_FILLET},
      {"GeometryNodeSDFGridLaplacian", ICON_GEO_N_SDF_GRID_LAPLACIAN},
      {"GeometryNodeSDFGridMean", ICON_GEO_N_SDF_GRID_MEAN},
      {"GeometryNodeSDFGridMeanCurvature", ICON_GEO_N_SDF_GRID_MEAN_CURVATURE},
      {"GeometryNodeSDFGridMedian", ICON_GEO_N_SDF_GRID_MEDIAN},
      {"GeometryNodeSDFGridOffset", ICON_GEO_N_SDF_GRID_OFFSET},
      {"GeometryNodeSampleCurve", ICON_GEO_N_SAMPLE_CURVE},
      {"GeometryNodeSampleGrid", ICON_GEO_N_SAMPLE_GRID},
      {"GeometryNodeSampleGridIndex", ICON_GEO_N_SAMPLE_GRID_INDEX},
      {"GeometryNodeSampleIndex", ICON_GEO_N_SAMPLE_INDEX},
      {"GeometryNodeSampleNearest", ICON_GEO_N_SAMPLE_NEAREST},
      {"GeometryNodeSampleNearestSurface", ICON_GEO_N_SAMPLE_NEAREST_SURFACE},
      {"GeometryNodeSampleUVSurface", ICON_GEO_N_SAMPLE_UV_SURFACE},
      {"GeometryNodeScaleElements", ICON_GEO_RESIZE},
      {"GeometryNodeScaleInstances", ICON_GEO_N_SCALE_INSTANCES},
      {"GeometryNodeSelfObject", ICON_GEO_N_SELF_OBJECT},
      {"GeometryNodeSeparateComponents", ICON_GEO_N_SEPARATE_COMPONENTS},
      {"GeometryNodeSeparateGeometry", ICON_GEO_N_SEPARATE_GEOMETRY},
      {"GeometryNodeSetCurveHandlePositions", ICON_GEO_N_SET_CURVE_HANDLE_POSITIONS},
      {"GeometryNodeSetCurveNormal", ICON_GEO_N_SET_CURVE_NORMAL},
      {"GeometryNodeSetCurveRadius", ICON_GEO_N_SET_CURVE_RADIUS},
      {"GeometryNodeSetCurveTilt", ICON_GEO_N_SET_CURVE_TILT},
      {"GeometryNodeSetGeometryBundle", ICON_GEO_N_SET_GEOMETRY_BUNDLE},
      {"GeometryNodeSetGeometryName", ICON_GEO_N_SET_GEOMETRY_NAME},
      {"GeometryNodeSetGreasePencilColor", ICON_GEO_N_SET_GREASE_PENCIL_COLOR},
      {"GeometryNodeSetGreasePencilDepth", ICON_GEO_N_SET_GREASE_PENCIL_DEPTH},
      {"GeometryNodeSetGreasePencilSoftness", ICON_GEO_N_SET_GREASE_PENCIL_SOFTNESS},
      {"GeometryNodeSetGridBackground", ICON_GEO_N_SET_GRID_BACKGROUND},
      {"GeometryNodeSetGridTransform", ICON_GEO_N_SET_GRID_TRANSFORM},
      {"GeometryNodeSetID", ICON_GEO_N_SET_ID},
      {"GeometryNodeSetInstanceTransform", ICON_GEO_N_SET_INSTANCE_TRANSFORM},
      {"GeometryNodeSetMaterial", ICON_GEO_MATERIAL},
      {"GeometryNodeSetMaterialIndex", ICON_GEO_N_SET_MATERIAL_INDEX},
      {"GeometryNodeSetMeshNormal", ICON_GEO_N_SET_MESH_NORMAL},
      {"GeometryNodeSetPointRadius", ICON_GEO_N_SET_POINT_RADIUS},
      {"GeometryNodeSetPosition", ICON_GEO_TRANSLATE},
      {"GeometryNodeSetShadeSmooth", ICON_GEO_N_SET_SHADE_SMOOTH},
      {"GeometryNodeSetSplineCyclic", ICON_GEO_N_SET_SPLINE_CYCLIC},
      {"GeometryNodeSetSplineResolution", ICON_GEO_N_SET_SPLINE_RESOLUTION},
      {"GeometryNodeSimulationInput", ICON_GEO_N_SIMULATION_INPUT},
      {"GeometryNodeSimulationOutput", ICON_GEO_N_SIMULATION_OUTPUT},
      {"GeometryNodeSmoothMesh", ICON_GEO_MOD_SMOOTH},
      {"GeometryNodeSortElements", ICON_GEO_N_SORT_ELEMENTS},
      {"GeometryNodeSplineLength", ICON_GEO_N_SPLINE_LENGTH},
      {"GeometryNodeSplineParameter", ICON_GEO_N_SPLINE_PARAMETER},
      {"GeometryNodeSplitEdges", ICON_GEO_MOD_EDGESPLIT},
      {"GeometryNodeSplitToInstances", ICON_GEO_N_SPLIT_TO_INSTANCES},
      {"GeometryNodeStoreNamedAttribute", ICON_GEO_N_STORE_NAMED_ATTRIBUTE},
      {"GeometryNodeStoreNamedGrid", ICON_GEO_N_STORE_NAMED_GRID},
      {"GeometryNodeStringJoin", ICON_GEO_N_STRING_JOIN},
      {"GeometryNodeStringToCurves", ICON_GEO_OUTLINER_OB_FONT},
      {"GeometryNodeSubdivideCurve", ICON_GEO_N_SUBDIVIDE_CURVE},
      {"GeometryNodeSubdivideMesh", ICON_GEO_N_SUBDIVIDE_MESH},
      {"GeometryNodeSubdivisionSurface", ICON_GEO_MOD_SUBSURF},
      {"GeometryNodeSweep", ICON_GEO_MOD_SWEEP},
      {"GeometryNodeSwitch", ICON_GEO_N_SWITCH},
      {"GeometryNodeTool3DCursor", ICON_GEO_N_TOOL3_D_CURSOR},
      {"GeometryNodeToolActiveElement", ICON_GEO_N_TOOL_ACTIVE_ELEMENT},
      {"GeometryNodeToolFaceSet", ICON_GEO_N_TOOL_FACE_SET},
      {"GeometryNodeToolMousePosition", ICON_GEO_N_TOOL_MOUSE_POSITION},
      {"GeometryNodeToolSelection", ICON_GEO_N_TOOL_SELECTION},
      {"GeometryNodeToolSetFaceSet", ICON_GEO_N_TOOL_SET_FACE_SET},
      {"GeometryNodeToolSetSelection", ICON_GEO_N_TOOL_SET_SELECTION},
      {"GeometryNodeTransform", ICON_GEO_TRANSFORM},
      {"GeometryNodeTranslateInstances", ICON_GEO_N_TRANSLATE_INSTANCES},
      {"GeometryNodeTriangulate", ICON_GEO_MOD_TRIANGULATE},
      {"GeometryNodeTrimCurve", ICON_GEO_N_TRIM_CURVE},
      {"GeometryNodeUVPackIslands", ICON_GEO_N_UV_PACK_ISLANDS},
      {"GeometryNodeUVTangent", ICON_GEO_N_UV_TANGENT},
      {"GeometryNodeUVUnwrap", ICON_GEO_UV},
      {"GeometryNodeVertexOfCorner", ICON_GEO_N_VERTEX_OF_CORNER},
      {"GeometryNodeViewer", ICON_GEO_N_VIEWER},
      {"GeometryNodeViewportTransform", ICON_GEO_N_VIEWPORT_TRANSFORM},
      {"GeometryNodeVolumeCube", ICON_GEO_N_VOLUME_CUBE},
      {"GeometryNodeVolumeToMesh", ICON_GEO_N_VOLUME_TO_MESH},
      {"GeometryNodeVoronoiFracture", ICON_GEO_MOD_VORONOI_FRACTURE},
      {"GeometryNodeWarning", ICON_GEO_N_WARNING},
      {"NodeClosureInput", ICON_GEO_N_CLOSURE_INPUT},
      {"NodeClosureOutput", ICON_GEO_N_CLOSURE_OUTPUT},
      {"NodeCombineBundle", ICON_GEO_N_COMBINE_BUNDLE},
      {"NodeEnableOutput", ICON_GEO_N_ENABLE_OUTPUT},
      {"NodeEvaluateClosure", ICON_GEO_N_EVALUATE_CLOSURE},
      {"NodeFrame", ICON_GEO_N_FRAME},
      {"NodeGetBundleItem", ICON_GEO_N_GET_BUNDLE_ITEM},
      {"NodeGroupInput", ICON_GEO_N_GROUP_INPUT},
      {"NodeGroupOutput", ICON_GEO_N_GROUP_OUTPUT},
      {"NodeJoinBundle", ICON_GEO_N_JOIN_BUNDLE},
      {"NodeReroute", ICON_GEO_N_REROUTE},
      {"NodeSeparateBundle", ICON_GEO_N_SEPARATE_BUNDLE},
      {"NodeStoreBundleItem", ICON_GEO_N_STORE_BUNDLE_ITEM},
      {"ShaderNodeBlackbody", ICON_GEO_N_BLACKBODY},
      {"ShaderNodeClamp", ICON_GEO_N_CLAMP},
      {"ShaderNodeCombineXYZ", ICON_GEO_N_COMBINE_XYZ},
      {"ShaderNodeFloatCurve", ICON_GEO_N_FLOAT_CURVE},
      {"ShaderNodeGamma", ICON_GEO_N_GAMMA},
      {"ShaderNodeMapRange", ICON_GEO_N_MAP_RANGE},
      {"ShaderNodeMath", ICON_GEO_N_MATH},
      {"ShaderNodeMix", ICON_GEO_N_MIX},
      {"ShaderNodeMixRGB", ICON_GEO_N_MIX_RGB},
      {"ShaderNodeRGBCurve", ICON_GEO_N_RGB_CURVE},
      {"ShaderNodeRadialTiling", ICON_GEO_N_RADIAL_TILING},
      {"ShaderNodeSeparateXYZ", ICON_GEO_N_SEPARATE_XYZ},
      {"ShaderNodeTexBrick", ICON_GEO_N_TEX_BRICK},
      {"ShaderNodeTexChecker", ICON_GEO_N_TEX_CHECKER},
      {"ShaderNodeTexGabor", ICON_GEO_N_TEX_GABOR},
      {"ShaderNodeTexGradient", ICON_GEO_N_TEX_GRADIENT},
      {"ShaderNodeTexMagic", ICON_GEO_N_TEX_MAGIC},
      {"ShaderNodeTexNoise", ICON_GEO_N_TEX_NOISE},
      {"ShaderNodeTexVoronoi", ICON_GEO_N_TEX_VORONOI},
      {"ShaderNodeTexWave", ICON_GEO_N_TEX_WAVE},
      {"ShaderNodeTexWhiteNoise", ICON_GEO_N_TEX_WHITE_NOISE},
      {"ShaderNodeValToRGB", ICON_GEO_N_VAL_TO_RGB},
      {"ShaderNodeValue", ICON_GEO_N_VALUE},
      {"ShaderNodeVectorCurve", ICON_GEO_N_VECTOR_CURVE},
      {"ShaderNodeVectorMath", ICON_GEO_N_VECTOR_MATH},
      {"ShaderNodeVectorRotate", ICON_GEO_N_VECTOR_ROTATE},
      /* Texture Graph nodes. */
      {"TextureGraphNodeChannelShuffle", ICON_NODE_TG_CHANNEL_SHUFFLE},
      {"TextureGraphNodeFXMap", ICON_NODE_TG_FX_MAP},
      {"TextureGraphNodePosition", ICON_NODE_TG_POSITION},
      /* Tree Graph nodes. */
      {"TreeGraphNodeOutput", ICON_NODE_TREE_OUTPUT},
      {"TreeGraphNodeTrunk", ICON_NODE_TREE_TRUNK},
      {"TreeGraphNodeBranches", ICON_NODE_TREE_BRANCHES},
      {"TreeGraphNodeLeaves", ICON_NODE_TREE_LEAVES},
      {"TreeGraphNodeRoots", ICON_NODE_TREE_ROOTS},
      {"TreeGraphNodeProfile", ICON_NODE_TREE_PROFILE},
      {"TreeGraphNodeAvoid", ICON_NODE_TREE_AVOID},
      {"TreeGraphNodeBranchInfo", ICON_NODE_TREE_BRANCH_INFO},
      {"TreeGraphNodeObjectInfo", ICON_NODE_TREE_OBJECT_INFO},
      {"TreeGraphNodePrune", ICON_NODE_TREE_PRUNE},
      {"TreeGraphNodeDrawn", ICON_NODE_TREE_DRAWN},
      {"TreeGraphNodeForce", ICON_NODE_TREE_FORCE},
      {"TreeGraphNodeCrown", ICON_NODE_TREE_CROWN},
      {"TreeGraphNodeBark", ICON_NODE_TREE_BARK},
      {"TreeGraphNodeFronds", ICON_NODE_TREE_FRONDS},
      {"TreeGraphNodeIvy", ICON_NODE_TREE_IVY},
      {"TextureGraphNodeRandom", ICON_NODE_TG_RANDOM},
      {"TextureGraphNodeSample", ICON_NODE_TG_SAMPLE},
      {"TextureGraphNodeDirectionalWarp", ICON_NODE_TG_DIRECTIONAL_WARP},
      {"TextureGraphNodeDistance", ICON_NODE_TG_DISTANCE},
      {"TextureGraphNodeEmboss", ICON_NODE_TG_EMBOSS},
      {"TextureGraphNodeHeightToNormal", ICON_NODE_TG_HEIGHT_TO_NORMAL},
      {"TextureGraphNodeLevels", ICON_NODE_TG_LEVELS},
      {"TextureGraphNodeSharpen", ICON_NODE_TG_SHARPEN},
      {"TextureGraphNodeSize", ICON_NODE_TG_SIZE},
      {"TextureGraphNodeSVG", ICON_NODE_TG_SVG},
      {"TextureGraphNodeText", ICON_NODE_TG_TEXT},
      {"TextureGraphNodeTilingNoise", ICON_NODE_TG_TILING_NOISE},
      {"TextureGraphNodeWarp", ICON_NODE_TG_WARP},
      {"TextureGraphNodeChannelMixer", ICON_NODE_TG_CHANNEL_MIXER},
      {"TextureGraphNodeChrominanceExtract", ICON_NODE_TG_CHROMINANCE_EXTRACT},
      {"TextureGraphNodeColorToMask", ICON_NODE_TG_COLOR_TO_MASK},
      {"TextureGraphNodeContrastLuminosity", ICON_NODE_TG_CONTRAST_LUMINOSITY},
      {"TextureGraphNodeGrayscaleConversion", ICON_NODE_TG_GRAYSCALE_CONVERSION},
      {"TextureGraphNodeHDRRangeViewer", ICON_NODE_TG_HDR_RANGE_VIEWER},
      {"TextureGraphNodeReplaceColor", ICON_NODE_TG_REPLACE_COLOR},
      {"TextureGraphNodeReplaceColorRange", ICON_NODE_TG_REPLACE_COLOR_RANGE},
      {"TextureGraphNodeThreshold", ICON_NODE_TG_THRESHOLD},
      {"TextureGraphNodeHistogramRange", ICON_NODE_TG_HISTOGRAM_RANGE},
      {"TextureGraphNodeHistogramScan", ICON_NODE_TG_HISTOGRAM_SCAN},
      {"TextureGraphNodeHistogramScanNonUniform", ICON_NODE_TG_HISTOGRAM_SCAN_NON_UNIFORM},
      {"TextureGraphNodeHistogramSelect", ICON_NODE_TG_HISTOGRAM_SELECT},
      {"TextureGraphNodeHistogramShift", ICON_NODE_TG_HISTOGRAM_SHIFT},
      {"TextureGraphNodeIDToMask", ICON_NODE_TG_ID_TO_MASK},
      {"TextureGraphNodeQuantizeGrayscale", ICON_NODE_TG_QUANTIZE_GRAYSCALE},
      {"TextureGraphNodeQuantizeColorSimple", ICON_NODE_TG_QUANTIZE_COLOR_SIMPLE},
      {"TextureGraphNodeAutoLevels", ICON_NODE_TG_AUTO_LEVELS},
      {"TextureGraphNodeMinMax", ICON_NODE_TG_MIN_MAX},
      {"TextureGraphNodeHistogramCompute", ICON_NODE_TG_HISTOGRAM_COMPUTE},
      {"TextureGraphNodeHistogramEqualize", ICON_NODE_TG_HISTOGRAM_EQUALIZE},
      {"TextureGraphNodeHistogramRender", ICON_NODE_TG_HISTOGRAM_RENDER},
      {"TextureGraphNodeHighpass", ICON_NODE_TG_HIGHPASS},
      {"TextureGraphNodeHeightFrequencies", ICON_NODE_TG_HEIGHT_FREQUENCIES},
      {"TextureGraphNodeLightingCancelHigh", ICON_NODE_TG_LIGHTING_CANCEL_HIGH},
      {"TextureGraphNodeLightingCancelLow", ICON_NODE_TG_LIGHTING_CANCEL_LOW},
      {"TextureGraphNodeLuminanceHighpass", ICON_NODE_TG_LUMINANCE_HIGHPASS},
      {"TextureGraphNodeHaldCLUT", ICON_NODE_TG_HALD_CLUT},
      {"TextureGraphNodeColorMatch", ICON_NODE_TG_COLOR_MATCH},
      {"TextureGraphNodeQuantizeColor", ICON_NODE_TG_QUANTIZE_COLOR},
      {"TextureGraphNodeApplyPalette", ICON_NODE_TG_APPLY_PALETTE},
      {"TextureGraphNodeCreatePalette", ICON_NODE_TG_CREATE_PALETTE},
      {"TextureGraphNodeModifyPalette", ICON_NODE_TG_MODIFY_PALETTE},
      {"TextureGraphNodeViewPalette", ICON_NODE_TG_VIEW_PALETTE},
      {"TextureGraphNodeDissolve", ICON_NODE_TG_DISSOLVE},
      {"TextureGraphNodeLinearBurn", ICON_NODE_TG_LINEAR_BURN},
      {"TextureGraphNodeAnisotropicBlur", ICON_NODE_TG_ANISOTROPIC_BLUR},
      {"TextureGraphNodeMedianFilter", ICON_NODE_TG_MEDIAN_FILTER},
      {"TextureGraphNodeMLVFilter", ICON_NODE_TG_MLV_FILTER},
      {"TextureGraphNodeNonUniformBlur", ICON_NODE_TG_NON_UNIFORM_BLUR},
      {"TextureGraphNodeRadialBlur", ICON_NODE_TG_RADIAL_BLUR},
      {"TextureGraphNodeSlopeBlur", ICON_NODE_TG_SLOPE_BLUR},
      {"TextureGraphNodePerlinNoise", ICON_NODE_TG_PERLIN_NOISE},
      {"TextureGraphNodeFractalSumBase", ICON_NODE_TG_FRACTAL_SUM_BASE},
      {"TextureGraphNodeFractalSum1", ICON_NODE_TG_FRACTAL_SUM_1},
      {"TextureGraphNodeFractalSum2", ICON_NODE_TG_FRACTAL_SUM_2},
      {"TextureGraphNodeFractalSum3", ICON_NODE_TG_FRACTAL_SUM_3},
      {"TextureGraphNodeFractalSum4", ICON_NODE_TG_FRACTAL_SUM_4},
      {"TextureGraphNodeClouds1", ICON_NODE_TG_CLOUDS_1},
      {"TextureGraphNodeClouds2", ICON_NODE_TG_CLOUDS_2},
      {"TextureGraphNodeClouds3", ICON_NODE_TG_CLOUDS_3},
      {"TextureGraphNodePlasma", ICON_NODE_TG_PLASMA},
      {"TextureGraphNodeCreased", ICON_NODE_TG_CREASED},
      {"TextureGraphNodeFluid", ICON_NODE_TG_FLUID},
      {"TextureGraphNodeLiquid", ICON_NODE_TG_LIQUID},
      {"TextureGraphNodeGaussianNoise", ICON_NODE_TG_GAUSSIAN_NOISE},
      {"TextureGraphNodeWhiteNoise", ICON_NODE_TG_WHITE_NOISE},
      {"TextureGraphNodeWhiteNoiseFast", ICON_NODE_TG_WHITE_NOISE_FAST},
      {"TextureGraphNodeBlueNoiseFast", ICON_NODE_TG_BLUE_NOISE_FAST},
      {"TextureGraphNodeAnisotropicNoise", ICON_NODE_TG_ANISOTROPIC_NOISE},
      {"TextureGraphNodeDirectionalNoise1", ICON_NODE_TG_DIRECTIONAL_NOISE_1},
      {"TextureGraphNodeDirectionalNoise2", ICON_NODE_TG_DIRECTIONAL_NOISE_2},
      {"TextureGraphNodeDirectionalNoise3", ICON_NODE_TG_DIRECTIONAL_NOISE_3},
      {"TextureGraphNodeDirectionalNoise4", ICON_NODE_TG_DIRECTIONAL_NOISE_4},
      {"TextureGraphNodeDirectionalScratches", ICON_NODE_TG_DIRECTIONAL_SCRATCHES},
      {"TextureGraphNodeCells1", ICON_NODE_TG_CELLS_1},
      {"TextureGraphNodeCells2", ICON_NODE_TG_CELLS_2},
      {"TextureGraphNodeCells3", ICON_NODE_TG_CELLS_3},
      {"TextureGraphNodeCells4", ICON_NODE_TG_CELLS_4},
      {"TextureGraphNodeVoronoi", ICON_NODE_TG_VORONOI},
      {"TextureGraphNodeVoronoiFractal", ICON_NODE_TG_VORONOI_FRACTAL},
      {"TextureGraphNodeCrystal1", ICON_NODE_TG_CRYSTAL_1},
      {"TextureGraphNodeCrystal2", ICON_NODE_TG_CRYSTAL_2},
      {"TextureGraphNodeCaustics", ICON_NODE_TG_CAUSTICS},
      {"TextureGraphNodeMicroscopeView", ICON_NODE_TG_MICROSCOPE_VIEW},
      {"TextureGraphNodeBnWSpots1", ICON_NODE_TG_BNW_SPOTS_1},
      {"TextureGraphNodeBnWSpots2", ICON_NODE_TG_BNW_SPOTS_2},
      {"TextureGraphNodeBnWSpots3", ICON_NODE_TG_BNW_SPOTS_3},
      {"TextureGraphNodeGaussianSpots1", ICON_NODE_TG_GAUSSIAN_SPOTS_1},
      {"TextureGraphNodeGaussianSpots2", ICON_NODE_TG_GAUSSIAN_SPOTS_2},
      {"TextureGraphNodeGrungeGalvanicLarge", ICON_NODE_TG_GRUNGE_GALVANIC_LARGE},
      {"TextureGraphNodeGrungeGalvanicSmall", ICON_NODE_TG_GRUNGE_GALVANIC_SMALL},
      {"TextureGraphNodeGrungeLeaks", ICON_NODE_TG_GRUNGE_LEAKS},
      {"TextureGraphNodeGrungeLeakyPaint", ICON_NODE_TG_GRUNGE_LEAKY_PAINT},
      {"TextureGraphNodeGrungeScratchesFine", ICON_NODE_TG_GRUNGE_SCRATCHES_FINE},
      {"TextureGraphNodeGrungeScratchesRough", ICON_NODE_TG_GRUNGE_SCRATCHES_ROUGH},
      {"TextureGraphNodeGrungeScratchesDirty", ICON_NODE_TG_GRUNGE_SCRATCHES_DIRTY},
      {"TextureGraphNodeGrungeShavings", ICON_NODE_TG_GRUNGE_SHAVINGS},
      {"TextureGraphNodeMessyFibers1", ICON_NODE_TG_MESSY_FIBERS_1},
      {"TextureGraphNodeMessyFibers2", ICON_NODE_TG_MESSY_FIBERS_2},
      {"TextureGraphNodeMessyFibers3", ICON_NODE_TG_MESSY_FIBERS_3},
      {"TextureGraphNodeFur1", ICON_NODE_TG_FUR_1},
      {"TextureGraphNodeFur2", ICON_NODE_TG_FUR_2},
      {"TextureGraphNodeFur3", ICON_NODE_TG_FUR_3},
      {"TextureGraphNodeWaveform1", ICON_NODE_TG_WAVEFORM_1},
      {"TextureGraphNodeDirtGradient", ICON_NODE_TG_DIRT_GRADIENT},
      {"TextureGraphNodeGrungeConcrete", ICON_NODE_TG_GRUNGE_CONCRETE},
      {"TextureGraphNodeGrungeDamas", ICON_NODE_TG_GRUNGE_DAMAS},
      {"TextureGraphNodeGrungeRoughDirty", ICON_NODE_TG_GRUNGE_ROUGH_DIRTY},
      {"TextureGraphNodeGrungeRustFine", ICON_NODE_TG_GRUNGE_RUST_FINE},
      {"TextureGraphNodeGrungeSpots", ICON_NODE_TG_GRUNGE_SPOTS},
      {"TextureGraphNodeGrungeSpotsDirty", ICON_NODE_TG_GRUNGE_SPOTS_DIRTY},
      {"TextureGraphNodeGrungeSplashesDusty", ICON_NODE_TG_GRUNGE_SPLASHES_DUSTY},
      {"TextureGraphNodeMoistureNoise1", ICON_NODE_TG_MOISTURE_NOISE_1},
      {"TextureGraphNodeMoistureNoise2", ICON_NODE_TG_MOISTURE_NOISE_2},
      {"TextureGraphNodeDirt1", ICON_NODE_TG_DIRT_1},
      {"TextureGraphNodeDirt2", ICON_NODE_TG_DIRT_2},
      {"TextureGraphNodeDirt3", ICON_NODE_TG_DIRT_3},
      {"TextureGraphNodeDirt4", ICON_NODE_TG_DIRT_4},
      {"TextureGraphNodeDirt5", ICON_NODE_TG_DIRT_5},
      {"TextureGraphNodeGrungeMap001", ICON_NODE_TG_GRUNGE_MAP_001},
      {"TextureGraphNodeGrungeMap002", ICON_NODE_TG_GRUNGE_MAP_002},
      {"TextureGraphNodeGrungeMap003", ICON_NODE_TG_GRUNGE_MAP_003},
      {"TextureGraphNodeGrungeMap004", ICON_NODE_TG_GRUNGE_MAP_004},
      {"TextureGraphNodeGrungeMap005", ICON_NODE_TG_GRUNGE_MAP_005},
      {"TextureGraphNodeGrungeMap006", ICON_NODE_TG_GRUNGE_MAP_006},
      {"TextureGraphNodeGrungeMap007", ICON_NODE_TG_GRUNGE_MAP_007},
      {"TextureGraphNodeGrungeMap008", ICON_NODE_TG_GRUNGE_MAP_008},
      {"TextureGraphNodeGrungeMap009", ICON_NODE_TG_GRUNGE_MAP_009},
      {"TextureGraphNodeGrungeMap010", ICON_NODE_TG_GRUNGE_MAP_010},
      {"TextureGraphNodeGrungeMap011", ICON_NODE_TG_GRUNGE_MAP_011},
      {"TextureGraphNodeGrungeMap012", ICON_NODE_TG_GRUNGE_MAP_012},
      {"TextureGraphNodeGrungeMap013", ICON_NODE_TG_GRUNGE_MAP_013},
      {"TextureGraphNodeGrungeMap014", ICON_NODE_TG_GRUNGE_MAP_014},
      {"TextureGraphNodeGrungeMap015", ICON_NODE_TG_GRUNGE_MAP_015},
      {"TextureGraphNodeAlveolus", ICON_NODE_TG_ALVEOLUS},
      {"TextureGraphNodeBrick1", ICON_NODE_TG_BRICK_1},
      {"TextureGraphNodeBrick2", ICON_NODE_TG_BRICK_2},
      {"TextureGraphNodeBrickGenerator", ICON_NODE_TG_BRICK_GENERATOR},
      {"TextureGraphNodeChecker1", ICON_NODE_TG_CHECKER_1},
      {"TextureGraphNodeFibers1", ICON_NODE_TG_FIBERS_1},
      {"TextureGraphNodeFibers2", ICON_NODE_TG_FIBERS_2},
      {"TextureGraphNodeGaussian1", ICON_NODE_TG_GAUSSIAN_1},
      {"TextureGraphNodeGaussian2", ICON_NODE_TG_GAUSSIAN_2},
      {"TextureGraphNodeGradientAxial", ICON_NODE_TG_GRADIENT_AXIAL},
      {"TextureGraphNodeGradientAxialReflected", ICON_NODE_TG_GRADIENT_AXIAL_REFLECTED},
      {"TextureGraphNodeGradientCircular", ICON_NODE_TG_GRADIENT_CIRCULAR},
      {"TextureGraphNodeGradientLinear1", ICON_NODE_TG_GRADIENT_LINEAR_1},
      {"TextureGraphNodeGradientLinear2", ICON_NODE_TG_GRADIENT_LINEAR_2},
      {"TextureGraphNodeGradientLinear3", ICON_NODE_TG_GRADIENT_LINEAR_3},
      {"TextureGraphNodeGradientRadial", ICON_NODE_TG_GRADIENT_RADIAL},
      {"TextureGraphNodeMesh1", ICON_NODE_TG_MESH_1},
      {"TextureGraphNodeMesh2", ICON_NODE_TG_MESH_2},
      {"TextureGraphNodePolygon1", ICON_NODE_TG_POLYGON_1},
      {"TextureGraphNodePolygon2", ICON_NODE_TG_POLYGON_2},
      {"TextureGraphNodeShape", ICON_NODE_TG_SHAPE},
      {"TextureGraphNodeStar", ICON_NODE_TG_STAR},
      {"TextureGraphNodeStarburst", ICON_NODE_TG_STARBURST},
      {"TextureGraphNodeStripes", ICON_NODE_TG_STRIPES},
      {"TextureGraphNodeWeave1", ICON_NODE_TG_WEAVE_1},
      {"TextureGraphNodeWeave2", ICON_NODE_TG_WEAVE_2},
      {"TextureGraphNodeWeaveGenerator", ICON_NODE_TG_WEAVE_GENERATOR},
      {"TextureGraphNodeTileGenerator", ICON_NODE_TG_TILE_GENERATOR},
      {"TextureGraphNodeTileSampler", ICON_NODE_TG_TILE_SAMPLER},
      {"TextureGraphNodeTileRandom", ICON_NODE_TG_TILE_RANDOM},
      {"TextureGraphNodeTileRandom2", ICON_NODE_TG_TILE_RANDOM_2},
      {"TextureGraphNodeSplatter", ICON_NODE_TG_SPLATTER},
      {"TextureGraphNodeSplatterCircular", ICON_NODE_TG_SPLATTER_CIRCULAR},
      {"TextureGraphNodeArcPavement", ICON_NODE_TG_ARC_PAVEMENT},
      {"TextureGraphNodeShapeMapper", ICON_NODE_TG_SHAPE_MAPPER},
      {"TextureGraphNodeScratchesGenerator", ICON_NODE_TG_SCRATCHES_GENERATOR},
      {"TextureGraphNodeShapeSplatter", ICON_NODE_TG_SHAPE_SPLATTER},
      {"TextureGraphNodeShapeSplatterBlend", ICON_NODE_TG_SHAPE_SPLATTER_BLEND},
      {"TextureGraphNodeShapeSplatterDataExtract", ICON_NODE_TG_SHAPE_SPLATTER_DATA_EXTRACT},
      {"TextureGraphNodeShapeSplatterToMask", ICON_NODE_TG_SHAPE_SPLATTER_TO_MASK},
      {"TextureGraphNodeHeightExtrude", ICON_NODE_TG_HEIGHT_EXTRUDE},
      {"TextureGraphNodeShapeExtrude", ICON_NODE_TG_SHAPE_EXTRUDE},
      {"TextureGraphNodePanoramaShape", ICON_NODE_TG_PANORAMA_SHAPE},
      {"TextureGraphNodeTriangleGrid", ICON_NODE_TG_TRIANGLE_GRID},
      {"TextureGraphNodeAmbientOcclusion", ICON_NODE_TG_AMBIENT_OCCLUSION},
      {"TextureGraphNodeBevel", ICON_NODE_TG_BEVEL},
      {"TextureGraphNodeCrossSection", ICON_NODE_TG_CROSS_SECTION},
      {"TextureGraphNodeCurvature", ICON_NODE_TG_CURVATURE},
      {"TextureGraphNodeCurvatureSmooth", ICON_NODE_TG_CURVATURE_SMOOTH},
      {"TextureGraphNodeCurvatureSobel", ICON_NODE_TG_CURVATURE_SOBEL},
      {"TextureGraphNodeEdgeDetect", ICON_NODE_TG_EDGE_DETECT},
      {"TextureGraphNodeNonUniformEdgeDetect", ICON_NODE_TG_NON_UNIFORM_EDGE_DETECT},
      {"TextureGraphNodeBlend", ICON_NODE_TG_BLEND},
      {"TextureGraphNodeHeightBlend", ICON_NODE_TG_HEIGHT_BLEND},
      {"TextureGraphNodeStoneWall", ICON_NODE_TG_STONE_WALL},
      {"TextureGraphNodeFittedWall", ICON_NODE_TG_FITTED_WALL},
      {"TextureGraphNodePackedWall", ICON_NODE_TG_PACKED_WALL},
      {"TextureGraphNodeDiffusionColor", ICON_NODE_TG_DIFFUSION_COLOR},
      {"TextureGraphNodeDiffusionGrayscale", ICON_NODE_TG_DIFFUSION_GRAYSCALE},
      {"TextureGraphNodeDiffusionUV", ICON_NODE_TG_DIFFUSION_UV},
      {"TextureGraphNodeEmbossWithGloss", ICON_NODE_TG_EMBOSS_WITH_GLOSS},
      {"TextureGraphNodeUberEmboss", ICON_NODE_TG_UBER_EMBOSS},
      {"TextureGraphNodeMosaic", ICON_NODE_TG_MOSAIC},
      {"TextureGraphNodeSummedAreaTable", ICON_NODE_TG_SUMMED_AREA_TABLE},
      {"TextureGraphNodeShadows", ICON_NODE_TG_SHADOWS},
      {"TextureGraphNodeExtendShape", ICON_NODE_TG_EXTEND_SHAPE},
      {"TextureGraphNodeShapeDropShadow", ICON_NODE_TG_SHAPE_DROP_SHADOW},
      {"TextureGraphNodeShapeGlow", ICON_NODE_TG_SHAPE_GLOW},
      {"TextureGraphNodeShapeStroke", ICON_NODE_TG_SHAPE_STROKE},
      {"TextureGraphNodeFloodFill", ICON_NODE_TG_FLOOD_FILL},
      {"TextureGraphNodeFloodFillMapper", ICON_NODE_TG_FLOOD_FILL_MAPPER},
      {"TextureGraphNodeFloodFillToBBoxSize", ICON_NODE_TG_FLOOD_FILL_TO_BBOX_SIZE},
      {"TextureGraphNodeFloodFillToGradient", ICON_NODE_TG_FLOOD_FILL_TO_GRADIENT},
      {"TextureGraphNodeFloodFillToColor", ICON_NODE_TG_FLOOD_FILL_TO_COLOR},
      {"TextureGraphNodeFloodFillToIndex", ICON_NODE_TG_FLOOD_FILL_TO_INDEX},
      {"TextureGraphNodeFloodFillToPosition", ICON_NODE_TG_FLOOD_FILL_TO_POSITION},
      {"TextureGraphNodeFloodFillToRandomColor", ICON_NODE_TG_FLOOD_FILL_TO_RANDOM_COLOR},
      {"TextureGraphNodeFloodFillToRandomGrayscale", ICON_NODE_TG_FLOOD_FILL_TO_RANDOM_GRAYSCALE},
      {"TextureGraphNodeMultiDirectionalWarp", ICON_NODE_TG_MULTI_DIRECTIONAL_WARP},
      {"TextureGraphNodeNonUniformDirectionalWarp", ICON_NODE_TG_NON_UNIFORM_DIRECTIONAL_WARP},
      {"TextureGraphNodeSwirl", ICON_NODE_TG_SWIRL},
      {"TextureGraphNodeVectorMorph", ICON_NODE_TG_VECTOR_MORPH},
      {"TextureGraphNodeVectorWarp", ICON_NODE_TG_VECTOR_WARP},
      {"TextureGraphNodeFacingNormal", ICON_NODE_TG_FACING_NORMAL},
      {"TextureGraphNodeHeightNormalBlender", ICON_NODE_TG_HEIGHT_NORMAL_BLENDER},
      {"TextureGraphNodeHeightToNormalWorldUnits", ICON_NODE_TG_HEIGHT_TO_NORMAL_WORLD_UNITS},
      {"TextureGraphNodeNormalBlend", ICON_NODE_TG_NORMAL_BLEND},
      {"TextureGraphNodeNormalCombine", ICON_NODE_TG_NORMAL_COMBINE},
      {"TextureGraphNodeNormalInvert", ICON_NODE_TG_NORMAL_INVERT},
      {"TextureGraphNodeNormalNormalize", ICON_NODE_TG_NORMAL_NORMALIZE},
      {"TextureGraphNodeNormalSobel", ICON_NODE_TG_NORMAL_SOBEL},
      {"TextureGraphNodeNormalToHeight", ICON_NODE_TG_NORMAL_TO_HEIGHT},
      {"TextureGraphNodeNormalToHeightHQ", ICON_NODE_TG_NORMAL_TO_HEIGHT_HQ},
      {"TextureGraphNodeNormalTransform", ICON_NODE_TG_NORMAL_TRANSFORM},
      {"TextureGraphNodeNormalVectorRotation", ICON_NODE_TG_NORMAL_VECTOR_ROTATION},
      {"TextureGraphNodeBentNormal", ICON_NODE_TG_BENT_NORMAL},
      {"TextureGraphNodeNormalUncombine", ICON_NODE_TG_NORMAL_UNCOMBINE},
      {"TextureGraphNodeCartesianToPolar", ICON_NODE_TG_CARTESIAN_TO_POLAR},
      {"TextureGraphNodePolarToCartesian", ICON_NODE_TG_POLAR_TO_CARTESIAN},
      {"TextureGraphNodeAutoCrop", ICON_NODE_TG_AUTO_CROP},
      {"TextureGraphNodeClone", ICON_NODE_TG_CLONE},
      {"TextureGraphNodeNoiseUpscale1", ICON_NODE_TG_NOISE_UPSCALE_1},
      {"TextureGraphNodeNoiseUpscale2", ICON_NODE_TG_NOISE_UPSCALE_2},
      {"TextureGraphNodeNoiseUpscale3", ICON_NODE_TG_NOISE_UPSCALE_3},
      {"TextureGraphNodeSafeTransform", ICON_NODE_TG_SAFE_TRANSFORM},
      {"TextureGraphNodeNonSquareTransform", ICON_NODE_TG_NON_SQUARE_TRANSFORM},
      {"TextureGraphNodeNonUniformRotation", ICON_NODE_TG_NON_UNIFORM_ROTATION},
      {"TextureGraphNodeSkew", ICON_NODE_TG_SKEW},
      {"TextureGraphNodeSymmetry", ICON_NODE_TG_SYMMETRY},
      {"TextureGraphNodeSymmetrySlice", ICON_NODE_TG_SYMMETRY_SLICE},
      {"TextureGraphNodeTrapezoidTransform", ICON_NODE_TG_TRAPEZOID_TRANSFORM},
      {"TextureGraphNodeMakeItTilePatch", ICON_NODE_TG_MAKE_IT_TILE_PATCH},
      {"TextureGraphNodeMakeItTilePhoto", ICON_NODE_TG_MAKE_IT_TILE_PHOTO},
      /* Compositor nodes the Texture Graph menus share. */
      {"CompositorNodeImage", ICON_NODE_TG_IMAGE},
      {"CompositorNodeImageInfo", ICON_NODE_TG_IMAGE_INFO},
      {"CompositorNodeImageCoordinates", ICON_NODE_TG_IMAGE_COORDINATES},
      {"CompositorNodeSceneTime", ICON_NODE_TG_SCENE_TIME},
      {"CompositorNodeRGB", ICON_NODE_TG_RGB},
      {"CompositorNodeViewer", ICON_NODE_TG_VIEWER},
      {"CompositorNodeAlphaOver", ICON_NODE_TG_ALPHA_OVER},
      {"CompositorNodeSetAlpha", ICON_NODE_TG_SET_ALPHA},
      {"CompositorNodeCombineColor", ICON_NODE_TG_COMBINE_COLOR},
      {"CompositorNodeSeparateColor", ICON_NODE_TG_SEPARATE_COLOR},
      {"CompositorNodeRGBToBW", ICON_NODE_TG_RGB_TO_BW},
      {"CompositorNodeInvert", ICON_NODE_TG_INVERT},
      {"CompositorNodeCurveRGB", ICON_NODE_TG_RGB_CURVES},
      {"CompositorNodeHueSat", ICON_NODE_TG_HUE_SATURATION},
      {"CompositorNodeBrightContrast", ICON_NODE_TG_BRIGHT_CONTRAST},
      {"CompositorNodeColorBalance", ICON_NODE_TG_COLOR_BALANCE},
      {"CompositorNodeExposure", ICON_NODE_TG_EXPOSURE},
      {"CompositorNodeHueCorrect", ICON_NODE_TG_HUE_CORRECT},
      {"CompositorNodePosterize", ICON_NODE_TG_POSTERIZE},
      {"CompositorNodeFilter", ICON_NODE_TG_FILTER},
      {"CompositorNodeDilateErode", ICON_NODE_TG_DILATE_ERODE},
      {"CompositorNodeDespeckle", ICON_NODE_TG_DESPECKLE},
      {"CompositorNodeKuwahara", ICON_NODE_TG_KUWAHARA},
      {"CompositorNodePixelate", ICON_NODE_TG_PIXELATE},
      {"CompositorNodeMaskToSDF", ICON_NODE_TG_MASK_TO_SDF},
      {"CompositorNodeInpaint", ICON_NODE_TG_INPAINT},
      {"CompositorNodeBlur", ICON_NODE_TG_BLUR},
      {"CompositorNodeDBlur", ICON_NODE_TG_DIRECTIONAL_BLUR},
      {"CompositorNodeBilateralblur", ICON_NODE_TG_BILATERAL_BLUR},
      {"CompositorNodeBokehBlur", ICON_NODE_TG_BOKEH_BLUR},
      {"CompositorNodeBoxMask", ICON_NODE_TG_BOX_MASK},
      {"CompositorNodeEllipseMask", ICON_NODE_TG_ELLIPSE_MASK},
      {"CompositorNodeTransform", ICON_NODE_TG_TRANSFORM},
      {"CompositorNodeTranslate", ICON_NODE_TG_TRANSLATE},
      {"CompositorNodeRotate", ICON_NODE_TG_ROTATE},
      {"CompositorNodeScale", ICON_NODE_TG_SCALE},
      {"CompositorNodeFlip", ICON_NODE_TG_FLIP},
      {"CompositorNodeCrop", ICON_NODE_TG_CROP},
      {"CompositorNodeCornerPin", ICON_NODE_TG_CORNER_PIN},
      {"CompositorNodeDisplace", ICON_NODE_TG_DISPLACE},
      {"CompositorNodeMapUV", ICON_NODE_TG_MAP_UV},
      {"CompositorNodeNormalize", ICON_NODE_TG_NORMALIZE},
      {"CompositorNodeRelativeToPixel", ICON_NODE_TG_RELATIVE_TO_PIXEL},
      {"CompositorNodeSwitch", ICON_NODE_TG_SWITCH},
      {"CompositorNodeNormal", ICON_NODE_TG_NORMAL},
  };
  for (const auto &item : node_icons) {
    bke::bNodeType *ntype = bke::node_type_find(item.idname);
    if (ntype == nullptr) {
      continue;
    }
    ntype->ui_icon = item.icon;
    /* Also for scripts, the Add menu reads it from there. */
    if (ntype->rna_ext.srna) {
      RNA_def_struct_ui_icon(ntype->rna_ext.srna, item.icon);
    }
  }
}

static void register_zone_types()
{
  static SimulationZoneType simulation_zone_type;
  static RepeatZoneType repeat_zone_type;
  static ForeachGeometryElementZoneType foreach_geometry_element_zone_type;
  static ClosureZoneType closure_zone_type;
  bke::register_node_zone_type(simulation_zone_type);
  bke::register_node_zone_type(repeat_zone_type);
  bke::register_node_zone_type(foreach_geometry_element_zone_type);
  bke::register_node_zone_type(closure_zone_type);
}

void register_nodes()
{
  register_zone_types();

  register_undefined_types();

  register_standard_node_socket_types();

  register_node_tree_type_geo();
  register_node_tree_type_cmp();
  register_node_tree_type_texture_graph();
  register_node_tree_type_tree_graph();

  register_node_type_frame();
  register_node_type_reroute();
  register_node_type_group_input();
  register_node_type_group_output();

  register_compositor_nodes();
  register_shader_nodes();
  register_texture_nodes();
  register_geometry_nodes();
  register_function_nodes();

  set_geometry_node_icons();
}

}  // namespace blender

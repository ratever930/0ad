/* Copyright (C) 2022 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "ActorViewer.h"

#include "View.h"

#include "graphics/Canvas2D.h"
#include "graphics/ColladaManager.h"
#include "graphics/LOSTexture.h"
#include "graphics/MiniMapTexture.h"
#include "graphics/Model.h"
#include "graphics/ModelDef.h"
#include "graphics/ObjectManager.h"
#include "graphics/ParticleManager.h"
#include "graphics/Patch.h"
#include "graphics/SkeletonAnimManager.h"
#include "graphics/Terrain.h"
#include "graphics/TerrainTextureEntry.h"
#include "graphics/TerrainTextureManager.h"
#include "graphics/TerritoryTexture.h"
#include "graphics/Unit.h"
#include "graphics/UnitManager.h"
#include "graphics/Overlay.h"
#include "maths/MathUtil.h"
#include "ps/Filesystem.h"
#include "ps/CLogger.h"
#include "ps/GameSetup/Config.h"
#include "ps/ProfileViewer.h"
#include "ps/VideoMode.h"
#include "renderer/backend/IDevice.h"
#include "renderer/backend/IDeviceCommandContext.h"
#include "renderer/Renderer.h"
#include "renderer/RenderingOptions.h"
#include "renderer/Scene.h"
#include "renderer/SceneRenderer.h"
#include "renderer/SkyManager.h"
#include "renderer/WaterManager.h"
#include "scriptinterface/ScriptContext.h"
#include "simulation2/Simulation2.h"
#include "simulation2/components/ICmpAttack.h"
#include "simulation2/components/ICmpOwnership.h"
#include "simulation2/components/ICmpPosition.h"
#include "simulation2/components/ICmpRangeManager.h"
#include "simulation2/components/ICmpTerrain.h"
#include "simulation2/components/ICmpUnitMotion.h"
#include "simulation2/components/ICmpVisual.h"
#include "simulation2/components/ICmpWaterManager.h"
#include "simulation2/helpers/Render.h"

extern int g_xres, g_yres;

struct ActorViewerImpl : public Scene
{
	NONCOPYABLE(ActorViewerImpl);
public:
	ActorViewerImpl() :
		Entity(INVALID_ENTITY),
		Terrain(),
		ColladaManager(g_VFS),
		MeshManager(ColladaManager),
		SkeletonAnimManager(ColladaManager),
		UnitManager(),
		Simulation2(&UnitManager, g_ScriptContext, &Terrain),
		ObjectManager(MeshManager, SkeletonAnimManager, Simulation2),
		LOSTexture(Simulation2),
		TerritoryTexture(Simulation2),
		MiniMapTexture(g_VideoMode.GetBackendDevice(), Simulation2)
	{
		UnitManager.SetObjectManager(ObjectManager);
	}

	entity_id_t Entity;
	CStrW CurrentUnitID;
	CStr CurrentUnitAnim;
	float CurrentSpeed;
	bool WalkEnabled;
	bool GroundEnabled;
	bool WaterEnabled;
	bool ShadowsEnabled;

	// Whether shadows, sky and water are enabled outside of the actor viewer.
	bool OldShadows;
	bool OldSky;
	bool OldWater;

	bool SelectionBoxEnabled;
	bool AxesMarkerEnabled;
	int PropPointsMode; // 0 disabled, 1 for point markers, 2 for point markers + axes

	CTerrain Terrain;

	CColladaManager ColladaManager;
	CMeshManager MeshManager;
	CSkeletonAnimManager SkeletonAnimManager;
	CUnitManager UnitManager;
	CSimulation2 Simulation2;
	CObjectManager ObjectManager; // Keep this after Simulation2 - it needs it for initialisation.
	CLOSTexture LOSTexture;
	CTerritoryTexture TerritoryTexture;
	CMiniMapTexture MiniMapTexture;

	SOverlayLine SelectionBoxOverlay;
	SOverlayLine AxesMarkerOverlays[3];
	std::vector<CModel::Prop> Props;
	std::vector<SOverlayLine> PropPointOverlays;

	// Simplistic implementation of the Scene interface
	virtual void EnumerateObjects(const CFrustum& frustum, SceneCollector* c)
	{
		if (GroundEnabled)
		{
			for (ssize_t pj = 0; pj < Terrain.GetPatchesPerSide(); ++pj)
				for (ssize_t pi = 0; pi < Terrain.GetPatchesPerSide(); ++pi)
					c->Submit(Terrain.GetPatch(pi, pj));
		}

		CmpPtr<ICmpVisual> cmpVisual(Simulation2, Entity);
		if (cmpVisual)
		{
			// add selection box outlines manually
			if (SelectionBoxEnabled)
			{
				SelectionBoxOverlay.m_Color = CColor(35/255.f, 86/255.f, 188/255.f, .75f); // pretty blue
				SelectionBoxOverlay.m_Thickness = 0.1f;

				SimRender::ConstructBoxOutline(cmpVisual->GetSelectionBox(), SelectionBoxOverlay);
				c->Submit(&SelectionBoxOverlay);
			}

			// add origin axis thingy
			if (AxesMarkerEnabled)
			{
				CMatrix3D worldSpaceAxes;
				// offset from the ground a little bit to prevent fighting with the floor texture (also note: SetTranslation
				// sets the identity 3x3 transformation matrix, which are the world axes)
				worldSpaceAxes.SetTranslation(cmpVisual->GetPosition() + CVector3D(0, 0.02f, 0));
				SimRender::ConstructAxesMarker(worldSpaceAxes, AxesMarkerOverlays[0], AxesMarkerOverlays[1], AxesMarkerOverlays[2]);

				c->Submit(&AxesMarkerOverlays[0]);
				c->Submit(&AxesMarkerOverlays[1]);
				c->Submit(&AxesMarkerOverlays[2]);
			}

			// add prop point overlays
			if (PropPointsMode > 0 && Props.size() > 0)
			{
				PropPointOverlays.clear(); // doesn't clear capacity, but should be ok since the number of prop points is usually pretty limited
				for (size_t i = 0; i < Props.size(); ++i)
				{
					CModel::Prop& prop = Props[i];
					if (prop.m_Model) // should always be the case
					{
						// prop point positions are automatically updated during animations etc. by CModel::ValidatePosition
						const CMatrix3D& propCoordSystem = prop.m_Model->GetTransform();

						SOverlayLine pointGimbal;
						pointGimbal.m_Color = CColor(1.f, 0.f, 1.f, 1.f);
						SimRender::ConstructGimbal(propCoordSystem.GetTranslation(), 0.05f, pointGimbal);
						PropPointOverlays.push_back(pointGimbal);

						if (PropPointsMode > 1)
						{
							// scale the prop axes coord system down a bit to distinguish them from the main world-space axes markers
							CMatrix3D displayCoordSystem = propCoordSystem;
							displayCoordSystem.Scale(0.5f, 0.5f, 0.5f);
							// revert translation scaling
							displayCoordSystem._14 = propCoordSystem._14;
							displayCoordSystem._24 = propCoordSystem._24;
							displayCoordSystem._34 = propCoordSystem._34;

							// construct an XYZ axes marker for the prop's coordinate system
							SOverlayLine xAxis, yAxis, zAxis;
							SimRender::ConstructAxesMarker(displayCoordSystem, xAxis, yAxis, zAxis);
							PropPointOverlays.push_back(xAxis);
							PropPointOverlays.push_back(yAxis);
							PropPointOverlays.push_back(zAxis);
						}
					}
				}

				for (size_t i = 0; i < PropPointOverlays.size(); ++i)
				{
					c->Submit(&PropPointOverlays[i]);
				}
			}
		}

		// send a RenderSubmit message so the components can submit their visuals to the renderer
		Simulation2.RenderSubmit(*c, frustum, false);
	}

	virtual CLOSTexture& GetLOSTexture()
	{
		return LOSTexture;
	}

	virtual CTerritoryTexture& GetTerritoryTexture()
	{
		return TerritoryTexture;
	}

	virtual CMiniMapTexture& GetMiniMapTexture()
	{
		return MiniMapTexture;
	}

	/**
	 * Recursively fetches the props of the currently displayed entity model and its submodels, and stores them for rendering.
	 */
	void UpdatePropList();
	void UpdatePropListRecursive(CModelAbstract* model);

};

void ActorViewerImpl::UpdatePropList()
{
	Props.clear();

	CmpPtr<ICmpVisual> cmpVisual(Simulation2, Entity);
	if (cmpVisual)
	{
		CUnit* unit = cmpVisual->GetUnit();
		if (unit)
		{
			CModelAbstract& modelAbstract = unit->GetModel();
			UpdatePropListRecursive(&modelAbstract);
		}
	}
}

void ActorViewerImpl::UpdatePropListRecursive(CModelAbstract* modelAbstract)
{
	ENSURE(modelAbstract);

	CModel* model = modelAbstract->ToCModel();
	if (model)
	{
		std::vector<CModel::Prop>& modelProps = model->GetProps();
		for (CModel::Prop& modelProp : modelProps)
		{
			Props.push_back(modelProp);
			if (modelProp.m_Model)
				UpdatePropListRecursive(modelProp.m_Model);
		}
	}
}

ActorViewer::ActorViewer()
	: m(*new ActorViewerImpl())
{
	m.WalkEnabled = false;
	m.GroundEnabled = true;
	m.WaterEnabled = false;
	m.ShadowsEnabled = g_RenderingOptions.GetShadows();
	m.SelectionBoxEnabled = false;
	m.AxesMarkerEnabled = false;
	m.PropPointsMode = 0;

	// Create a tiny empty piece of terrain, just so we can put shadows
	// on it without having to think too hard
	m.Terrain.Initialize(2, NULL);
	CTerrainTextureEntry* tex = g_TexMan.FindTexture("whiteness");
	if (tex)
	{
		for (ssize_t pi = 0; pi < m.Terrain.GetPatchesPerSide(); ++pi)
		{
			for (ssize_t pj = 0; pj < m.Terrain.GetPatchesPerSide(); ++pj)
			{
				CPatch* patch = m.Terrain.GetPatch(pi, pj);
				for (ssize_t i = 0; i < PATCH_SIZE; ++i)
				{
					for (ssize_t j = 0; j < PATCH_SIZE; ++j)
					{
						CMiniPatch& mp = patch->m_MiniPatches[i][j];
						mp.Tex = tex;
						mp.Priority = 0;
					}
				}
			}
		}
	}
	else
	{
		debug_warn(L"Failed to load whiteness texture");
	}

	// Prepare the simulation
	m.Simulation2.LoadDefaultScripts();
	m.Simulation2.ResetState();

	// Set player data
	m.Simulation2.SetMapSettings(m.Simulation2.GetPlayerDefaults());
	m.Simulation2.LoadPlayerSettings(true);

	// Tell the simulation we've already loaded the terrain
	CmpPtr<ICmpTerrain> cmpTerrain(m.Simulation2, SYSTEM_ENTITY);
	if (cmpTerrain)
		cmpTerrain->ReloadTerrain(false);

	// Remove FOW since we're in Atlas
	CmpPtr<ICmpRangeManager> cmpRangeManager(m.Simulation2, SYSTEM_ENTITY);
	if (cmpRangeManager)
		cmpRangeManager->SetLosRevealAll(-1, true);

	m.Simulation2.InitGame();
}

ActorViewer::~ActorViewer()
{
	delete &m;
}

CSimulation2* ActorViewer::GetSimulation2()
{
	return &m.Simulation2;
}

entity_id_t ActorViewer::GetEntity()
{
	return m.Entity;
}

void ActorViewer::UnloadObjects()
{
	m.ObjectManager.UnloadObjects();
}

void ActorViewer::SetActor(const CStrW& name, const CStr& animation, player_id_t playerID)
{
	bool needsAnimReload = false;

	CStrW id = name;

	// Recreate the entity, if we don't have one or if the new one is different
	if (m.Entity == INVALID_ENTITY || id != m.CurrentUnitID)
	{
		// Delete the old entity (if any)
		if (m.Entity != INVALID_ENTITY)
		{
			m.Simulation2.DestroyEntity(m.Entity);
			m.Simulation2.FlushDestroyedEntities();
			m.Entity = INVALID_ENTITY;
		}

		// Clear particles associated with deleted entity
		g_Renderer.GetSceneRenderer().GetParticleManager().ClearUnattachedEmitters();

		// If there's no actor to display, return with nothing loaded
		if (id.empty())
			return;

		m.Entity = m.Simulation2.AddEntity(L"preview|" + id);
		if (m.Entity == INVALID_ENTITY)
			return;

		CmpPtr<ICmpPosition> cmpPosition(m.Simulation2, m.Entity);
		if (cmpPosition)
		{
			ssize_t c = TERRAIN_TILE_SIZE * m.Terrain.GetPatchesPerSide()*PATCH_SIZE/2;
			cmpPosition->JumpTo(entity_pos_t::FromInt(c), entity_pos_t::FromInt(c));
			cmpPosition->SetYRotation(entity_angle_t::Pi());
		}

		CmpPtr<ICmpOwnership> cmpOwnership(m.Simulation2, m.Entity);
		if (cmpOwnership)
			cmpOwnership->SetOwner(playerID);

		needsAnimReload = true;
	}

	if (animation != m.CurrentUnitAnim)
		needsAnimReload = true;

	if (needsAnimReload)
	{
		// Emulate the typical simulation animation behaviour.
		CStr anim = animation.LowerCase();
		float speed = 1.0f;
		// Speed will be ignored if we have a repeat time.
		float repeatTime = 0.0f;
		m.CurrentSpeed = 0.0f;
		if (anim == "walk")
		{
			CmpPtr<ICmpUnitMotion> cmpUnitMotion(m.Simulation2, m.Entity);
			if (cmpUnitMotion)
				speed  = cmpUnitMotion->GetWalkSpeed().ToFloat();
			else
				speed = 7.f; // Typical unit walk speed.
			m.CurrentSpeed = speed;
		}
		else if (anim == "run")
		{
			CmpPtr<ICmpUnitMotion> cmpUnitMotion(m.Simulation2, m.Entity);
			if (cmpUnitMotion)
				speed = cmpUnitMotion->GetWalkSpeed().ToFloat() * cmpUnitMotion->GetRunMultiplier().ToFloat();
			else
				speed = 12.f; // Typical unit run speed.

			m.CurrentSpeed = speed;
		}
		else if (anim.Find("attack_") == 0)
		{
			CmpPtr<ICmpAttack> cmpAttack(m.Simulation2, m.Entity);
			if (cmpAttack)
				for (const CStr& type : cmpAttack->GetAttackTypes())
					if (anim == "attack_" + type.LowerCase())
					{
						repeatTime = GetRepeatTimeByAttackType(type);
						break;
					}
		}

		CmpPtr<ICmpVisual> cmpVisual(m.Simulation2, m.Entity);
		if (cmpVisual)
		{
			// TODO: SetEntitySelection(anim)
			cmpVisual->SelectAnimation(anim, false, fixed::FromFloat(speed));
			if (repeatTime > 0.0f)
				cmpVisual->SetAnimationSyncRepeat(fixed::FromFloat(repeatTime));
		}

		// update prop list for new entity/animation (relies on needsAnimReload also getting called for entire entity changes)
		m.UpdatePropList();
	}

	m.CurrentUnitID = id;
	m.CurrentUnitAnim = animation;
}

void ActorViewer::SetEnabled(bool enabled)
{
	if (enabled)
	{
		// Set shadows, sky and water.
		m.OldShadows = g_RenderingOptions.GetShadows();
		SetShadowsEnabled(m.ShadowsEnabled);

		m.OldSky = g_Renderer.GetSceneRenderer().GetSkyManager().IsSkyVisible();
		g_Renderer.GetSceneRenderer().GetSkyManager().SetSkyVisible(false);

		m.OldWater = g_Renderer.GetSceneRenderer().GetWaterManager().m_RenderWater;
		g_Renderer.GetSceneRenderer().GetWaterManager().m_RenderWater = m.WaterEnabled;
	}
	else
	{
		// Restore the old renderer state
		SetShadowsEnabled(m.OldShadows);
		g_Renderer.GetSceneRenderer().GetSkyManager().SetSkyVisible(m.OldSky);
		g_Renderer.GetSceneRenderer().GetWaterManager().m_RenderWater = m.OldWater;
	}
}

void ActorViewer::SetWalkEnabled(bool enabled)    { m.WalkEnabled = enabled; }
void ActorViewer::SetGroundEnabled(bool enabled)  { m.GroundEnabled = enabled; }
void ActorViewer::SetWaterEnabled(bool enabled)
{
	m.WaterEnabled = enabled;
	// Adjust water level
	entity_pos_t waterLevel = entity_pos_t::FromFloat(enabled ? 10.f : 0.f);
	CmpPtr<ICmpWaterManager> cmpWaterManager(m.Simulation2, SYSTEM_ENTITY);
	if (cmpWaterManager)
		cmpWaterManager->SetWaterLevel(waterLevel);
}
void ActorViewer::SetShadowsEnabled(bool enabled) {
	g_RenderingOptions.SetShadows(enabled);
	m.ShadowsEnabled = enabled;
}
void ActorViewer::ToggleShadows()
{
	SetShadowsEnabled(!m.ShadowsEnabled);
}
void ActorViewer::SetBoundingBoxesEnabled(bool enabled) { m.SelectionBoxEnabled = enabled; }
void ActorViewer::SetAxesMarkerEnabled(bool enabled)    { m.AxesMarkerEnabled = enabled; }
void ActorViewer::SetPropPointsMode(int mode)           { m.PropPointsMode = mode; }

void ActorViewer::SetStatsEnabled(bool enabled)
{
	if (enabled)
		g_ProfileViewer.ShowTable("renderer");
	else
		g_ProfileViewer.ShowTable("");
}

float ActorViewer::GetRepeatTimeByAttackType(const std::string& type) const
{
	CmpPtr<ICmpAttack> cmpAttack(m.Simulation2, m.Entity);
	if (cmpAttack)
		return cmpAttack->GetRepeatTime(type);

	return 0.0f;
}

void ActorViewer::Render()
{
	// TODO: ActorViewer should reuse CRenderer code and not duplicate it.

	CSceneRenderer& sceneRenderer = g_Renderer.GetSceneRenderer();

	// Set simulation context for rendering purposes
	sceneRenderer.SetSimulation(&m.Simulation2);

	// Find the centre of the interesting region, in the middle of the patch
	// and half way up the model (assuming there is one)
	CVector3D centre;
	CmpPtr<ICmpVisual> cmpVisual(m.Simulation2, m.Entity);
	if (cmpVisual)
		cmpVisual->GetBounds().GetCenter(centre);
	else
		centre.Y = 0.f;
	centre.X = centre.Z = TERRAIN_TILE_SIZE * m.Terrain.GetPatchesPerSide()*PATCH_SIZE/2;

	CCamera camera = AtlasView::GetView_Actor()->GetCamera();
	camera.m_Orientation.Translate(centre.X, centre.Y, centre.Z);
	camera.UpdateFrustum();

	sceneRenderer.SetSceneCamera(camera, camera);

	g_Renderer.BeginFrame();

	Renderer::Backend::IDeviceCommandContext* deviceCommandContext =
		g_Renderer.GetDeviceCommandContext();

	sceneRenderer.PrepareScene(deviceCommandContext, m);

	Renderer::Backend::IFramebuffer* backbuffer =
		deviceCommandContext->GetDevice()->GetCurrentBackbuffer(
			Renderer::Backend::AttachmentLoadOp::DONT_CARE,
			Renderer::Backend::AttachmentStoreOp::STORE,
			Renderer::Backend::AttachmentLoadOp::CLEAR,
			Renderer::Backend::AttachmentStoreOp::DONT_CARE);
	deviceCommandContext->BeginFramebufferPass(backbuffer);

	Renderer::Backend::IDeviceCommandContext::Rect viewportRect{};
	viewportRect.width = backbuffer->GetWidth();
	viewportRect.height = backbuffer->GetHeight();
	deviceCommandContext->SetViewports(1, &viewportRect);

	sceneRenderer.RenderScene(deviceCommandContext);
	sceneRenderer.RenderSceneOverlays(deviceCommandContext);

	{
		CCanvas2D canvas(g_xres, g_yres, g_VideoMode.GetScale(), deviceCommandContext);
		g_Logger->Render(canvas);
		g_ProfileViewer.RenderProfile(canvas);
	}

	deviceCommandContext->EndFramebufferPass();

	g_Renderer.EndFrame();
}

void ActorViewer::Update(float simFrameLength, float realFrameLength)
{
	m.Simulation2.Update((int)(simFrameLength*1000));
	m.Simulation2.Interpolate(simFrameLength, 0, realFrameLength);

	if (m.WalkEnabled && m.CurrentSpeed)
	{
		CmpPtr<ICmpPosition> cmpPosition(m.Simulation2, m.Entity);
		if (cmpPosition)
		{
			// Move the model by speed*simFrameLength forwards
			float z = cmpPosition->GetPosition().Z.ToFloat();
			z -= m.CurrentSpeed*simFrameLength;
			// Wrap at the edges, so it doesn't run off into the horizon
			ssize_t c = TERRAIN_TILE_SIZE * m.Terrain.GetPatchesPerSide()*PATCH_SIZE/2;
			if (z < c - TERRAIN_TILE_SIZE*PATCH_SIZE * 0.1f)
				z = c + TERRAIN_TILE_SIZE*PATCH_SIZE * 0.1f;
			cmpPosition->JumpTo(cmpPosition->GetPosition().X, entity_pos_t::FromFloat(z));
		}
	}
}

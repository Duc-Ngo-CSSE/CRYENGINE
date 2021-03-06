// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "Fog.h"

#include "DriverD3D.h"
#include "D3DPostProcess.h"
#include "D3D_SVO.h"

#include "Common/ReverseDepth.h"
#include "GraphicsPipeline/VolumetricFog.h"

namespace
{
inline float expf_s(float arg)
{
	return expf(clamp_tpl(arg, -80.0f, 80.0f));
}

inline float MaxChannel(const Vec4& col)
{
	return max(max(col.x, col.y), col.z);
}
}

CFogStage::CFogStage()
	: m_pTexInterleaveSamplePattern(nullptr)
	, m_samplerPointClamp(-1)
{
}

CFogStage::~CFogStage()
{
	SAFE_RELEASE(m_pTexInterleaveSamplePattern);
}

void CFogStage::Init()
{
	m_pTexInterleaveSamplePattern = CTexture::ForName("EngineAssets/Textures/FogVolShadowJitter.tif", FT_DONT_STREAM | FT_NOMIPS, eTF_Unknown);

	m_samplerPointClamp = CTexture::GetTexState(STexState(FILTER_POINT, true));
}

void CFogStage::Prepare(CRenderView* pRenderView)
{
#if defined(VOLUMETRIC_FOG_SHADOWS)
	CD3D9Renderer* const __restrict rd = gcpRendD3D;
	int32 screenWidth = rd->GetWidth();
	int32 screenHeight = rd->GetHeight();

	// Recreate render targets if quality was changed
	bool halfRes = (CRenderer::CV_r_FogShadows == 1);
	bool quarterRes = (CRenderer::CV_r_FogShadows == 2);
	if ((halfRes && CTexture::s_ptexVolFogShadowBuf[0]->GetWidth() != screenWidth / 2)
	    || (quarterRes && CTexture::s_ptexVolFogShadowBuf[0]->GetWidth() != screenWidth / 4))
	{
		int32 width = screenWidth / (halfRes ? 2 : 4);
		int32 height = screenHeight / (halfRes ? 2 : 4);
		for (uint32 i = 0; i < 2; ++i)
		{
			ETEX_Format fmt = CTexture::s_ptexVolFogShadowBuf[i]->GetDstFormat();
			CTexture::s_ptexVolFogShadowBuf[i]->Invalidate(width, height, fmt);
			CTexture::s_ptexVolFogShadowBuf[i]->CreateRenderTarget(fmt, Clr_Transparent);
		}
	}
#endif
}

void CFogStage::Execute()
{
	CD3D9Renderer* const __restrict rd = gcpRendD3D;
	const SRenderPipeline& rp(rd->m_RP);
	const SThreadInfo* const pShaderThreadInfo = &(rp.m_TI[rp.m_nProcessThreadID]);

	if (!(pShaderThreadInfo->m_FS.m_bEnable && CRenderer::CV_r_usezpass != 0))
	{
		return;
	}

	// MSAA can't be used in new graphics pipeline.
	CRY_ASSERT((rp.m_PersFlags2 & (RBPF2_MSAA_STENCILCULL | RBPF2_MSAA_SAMPLEFREQ_PASS)) == 0);

	PROFILE_LABEL_SCOPE("FOG_GLOBAL");

	Prepare(RenderView());

	SDepthTexture& depthBuffer = rd->m_DepthBufferOrigMSAA;
	int32 width = rd->GetWidth();
	int32 height = rd->GetHeight();
	const bool bReverseDepth = (rp.m_TI[rp.m_nProcessThreadID].m_PersFlags & RBPF_REVERSE_DEPTH) != 0;
	const bool bVolumtricFog = (rd->m_bVolumetricFogEnabled != 0);
#if defined(VOLUMETRIC_FOG_SHADOWS)
	const bool bVolFogShadow = (rd->m_bVolFogShadowsEnabled) && !bVolumtricFog;
#else
	const bool bVolFogShadow = false;
#endif
	auto* pVolFogStage = rd->GetGraphicsPipeline().GetVolumetricFogStage();

	if (bVolFogShadow)
	{
		ExecuteVolumetricFogShadow();
	}

	CShader* pShader = CShaderMan::s_shHDRPostProcess;

	// render fog
	{
#if defined(FEATURE_SVO_GI)
		// activate support for SVO atmosphere in fog shader
		CSvoRenderer* pSR = CSvoRenderer::GetInstance();
		const bool bSVOGI =
		  (pSR
		   && pSR->GetTroposphereMinRT()
		   && pSR->GetTroposphereShadRT()
		   && pSR->GetTroposphereMaxRT());
#else
		const bool bSVOGI = false;
#endif

		// prevent fog depth clipping if volumetric fog or SVOGI are activated.
		const bool useFogDepthTest = (CRenderer::CV_r_FogDepthTest != 0.0f)
		                             && !bVolumtricFog
		                             && !bSVOGI;

		f32 fogDepth = 0.0f;
		if (useFogDepthTest)
		{
			fogDepth = GetFogCullDistance();
		}
		rd->m_fogCullDistance = fogDepth;

		uint32 inputFlag = 0;
		inputFlag |= bVolFogShadow ? BIT(0) : 0;
		inputFlag |= bSVOGI ? BIT(1) : 0;
		inputFlag |= bVolumtricFog ? BIT(2) : 0;
		inputFlag |= useFogDepthTest ? BIT(3) : 0;
		inputFlag |= bReverseDepth ? BIT(4) : 0;

		if (m_passFog.InputChanged(inputFlag, CTexture::s_ptexHDRTarget->GetTextureID()))
		{
			uint64 rtMask = 0;
			rtMask |= bVolFogShadow ? g_HWSR_MaskBit[HWSR_SAMPLE0] : 0;
			rtMask |= bSVOGI ? g_HWSR_MaskBit[HWSR_SAMPLE2] : 0;
			rtMask |= bVolumtricFog ? g_HWSR_MaskBit[HWSR_VOLUMETRIC_FOG] : 0;

			static CCryNameTSCRC techName("FogPass");
			m_passFog.SetTechnique(pShader, techName, rtMask);
			m_passFog.SetRenderTarget(0, CTexture::s_ptexHDRTarget);
			m_passFog.SetDepthTarget(&depthBuffer);

			// using GS_BLDST_SRCALPHA because GS_BLDST_ONEMINUSSRCALPHA causes banding artifact when alpha value is very low.
			uint32 nRS = GS_BLSRC_ONE | GS_BLDST_SRCALPHA | (useFogDepthTest ? GS_DEPTHFUNC_LEQUAL : GS_NODEPTHTEST);
			nRS = bReverseDepth ? ReverseDepthHelper::ConvertDepthFunc(nRS) : nRS;
			m_passFog.SetState(nRS);

			m_passFog.SetTexture(0, CTexture::s_ptexZTarget);

			if (bVolumtricFog)
			{
				pVolFogStage->BindVolumetricFogResources(m_passFog, 1, 0);
			}
#if defined(VOLUMETRIC_FOG_SHADOWS)
			else if (bVolFogShadow)
			{
				m_passFog.SetTextureSamplerPair(1, CTexture::s_ptexVolFogShadowBuf[0], m_samplerPointClamp);
			}
#endif
#if defined(FEATURE_SVO_GI)
			else if (bSVOGI)
			{
				// bind SVO atmosphere
				m_passFog.SetTextureSamplerPair(12, pSR->GetTroposphereMinRT(), m_samplerPointClamp);
				m_passFog.SetTextureSamplerPair(13, pSR->GetTroposphereShadRT(), m_samplerPointClamp);
				m_passFog.SetTextureSamplerPair(14, pSR->GetTroposphereMaxRT(), m_samplerPointClamp);
			}
#endif
		}

		f32 clipZ = 0.0f;
		if (useFogDepthTest)
		{
			const Matrix44A& projMat = *(pShaderThreadInfo->m_matProj->GetTop());

			// projMat.m23 is -1 or 1 depending on whether we use a RH or LH coord system
			// done in favor of an if check to make homogeneous divide by fogDepth (which is always positive) work
			clipZ = projMat.m23 * fogDepth * projMat.m22 + projMat.m32;
			clipZ /= fogDepth;
			clipZ = clamp_tpl(clipZ, 0.0f, 1.0f);

			// reverse depth operation is needed here because CFullscreenPass's internal reverse operation counteracts projMat's reverse operation.
			clipZ = bReverseDepth ? (1.0f - clipZ) : clipZ;
		}

		m_passFog.SetRequireWorldPos(true, clipZ); // don't forget if shader reconstructs world position.

		m_passFog.BeginConstantUpdate();

		const f32 averageLuminance = rd->m_vSceneLuminanceInfo.x;
		const f32 adaptedSceneScale = rd->m_fAdaptedSceneScale;
		static CCryNameR szHDRParams("HDRParams2");
		m_passFog.SetConstant(eHWSC_Pixel, szHDRParams, Vec4(averageLuminance, adaptedSceneScale, 0, 0));

#if defined(VOLUMETRIC_FOG_SHADOWS)
		if (bVolFogShadow)
		{
			Vec3 volFogShadowDarkeningP;
			gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_VOLFOG_SHADOW_DARKENING, volFogShadowDarkeningP);

			static CCryNameR volFogShadowDarkeningN("volFogShadowDarkening");
			m_passFog.SetConstant(eHWSC_Pixel, volFogShadowDarkeningN, Vec4(volFogShadowDarkeningP, 0));

			const float aSun = (1.0f - clamp_tpl(volFogShadowDarkeningP.y, 0.0f, 1.0f)) * 1.0f;
			const float bSun = 1.0f - aSun;
			const float aAmb = (1.0f - clamp_tpl(volFogShadowDarkeningP.z, 0.0f, 1.0f)) * 0.4f;
			const float bAmb = 1.0f - aAmb;

			static CCryNameR volFogShadowDarkeningSunAmbN("volFogShadowDarkeningSunAmb");
			m_passFog.SetConstant(eHWSC_Pixel, volFogShadowDarkeningSunAmbN, Vec4(aSun, bSun, aAmb, bAmb));

			Vec4 volFogShadowRange;
			{
				Vec3 volFogShadowRangeP;
				gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_VOLFOG_SHADOW_RANGE, volFogShadowRangeP);
				volFogShadowRangeP.x = clamp_tpl(volFogShadowRangeP.x, 0.01f, 1.0f);
				volFogShadowRange = Vec4(volFogShadowRangeP.x, 1.0f / volFogShadowRangeP.x, 0, 0);
			}
			static CCryNameR volFogShadowRangeN("volFogShadowRange");
			m_passFog.SetConstant(eHWSC_Pixel, volFogShadowRangeN, volFogShadowRange);

			Vec4 sampleOffsets[5];
			{
				const float tU = 1.0f / static_cast<float>(CTexture::s_ptexVolFogShadowBuf[0]->GetWidth());
				const float tV = 1.0f / static_cast<float>(CTexture::s_ptexVolFogShadowBuf[0]->GetHeight());

				sampleOffsets[0] = Vec4(0, 0, 0, 0);
				sampleOffsets[1] = Vec4(0, -tV, 0, 0);
				sampleOffsets[2] = Vec4(-tU, 0, 0, 0);
				sampleOffsets[3] = Vec4(tU, 0, 0, 0);
				sampleOffsets[4] = Vec4(0, tU, 0, 0);
			}
			static CCryNameR volFogShadowBufSampleOffsetsN("volFogShadowBufSampleOffsets");
			m_passFog.SetConstantArray(eHWSC_Pixel, volFogShadowBufSampleOffsetsN, sampleOffsets, 5);
		}
#endif

#if defined(FEATURE_SVO_GI)
		static CCryNameR sSVO_AirTextureScale("SVO_AirTextureScale");
		Vec4 vSVO_AirTextureScale(
		  bSVOGI ? float(width / pSR->GetTroposphereMinRT()->GetWidth()) : 0.0f,
		  bSVOGI ? float(height / pSR->GetTroposphereMinRT()->GetHeight()) : 0.0f,
		  0.0f, 0.0f);
		m_passFog.SetConstant(eHWSC_Pixel, sSVO_AirTextureScale, vSVO_AirTextureScale);
#endif

		m_passFog.Execute();
	}

	Vec3 lCol;
	gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_SKY_HIGHLIGHT_COLOR, lCol);
	bool bLightning = (lCol.x > 1e-4f || lCol.y > 1e-4f || lCol.z > 1e-4f);

	// render fog with lightning
	if (bLightning)
	{
		if (m_passLightning.InputChanged())
		{
			uint64 rtMask = 0;

			static CCryNameTSCRC techName("FogPassWithLightning");
			m_passLightning.SetTechnique(pShader, techName, rtMask);
			m_passLightning.SetRenderTarget(0, CTexture::s_ptexHDRTarget);

			m_passLightning.SetState(GS_NODEPTHTEST | GS_BLSRC_ONE | GS_BLDST_ONE);

			m_passLightning.SetTexture(0, CTexture::s_ptexZTarget);

			m_passLightning.SetRequireWorldPos(true);
		}

		m_passLightning.BeginConstantUpdate();

		Vec3 lPos;
		gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_SKY_HIGHLIGHT_POS, lPos);
		static CCryNameR Param1Name("LightningPos");
		m_passLightning.SetConstant(eHWSC_Pixel, Param1Name, Vec4(lPos.x, lPos.y, lPos.z, 0.0f));

		Vec3 lSize;
		gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_SKY_HIGHLIGHT_SIZE, lSize);
		static CCryNameR Param2Name("LightningColSize");
		m_passLightning.SetConstant(eHWSC_Pixel, Param2Name, Vec4(lCol.x, lCol.y, lCol.z, lSize.x * 0.01f));

		m_passLightning.Execute();
	}
}

void CFogStage::ExecuteVolumetricFogShadow()
{
#if defined(VOLUMETRIC_FOG_SHADOWS)
	CD3D9Renderer* const __restrict rd = gcpRendD3D;

	const bool bCloudShadow = rd->m_bVolFogCloudShadowsEnabled;
	const bool bVolCloudShadow = bCloudShadow && (rd->m_bVolumetricCloudsEnabled);

	CShader* pShader = CShaderMan::s_shHDRPostProcess;

	// ray cast into sun shadow maps
	{
		uint64 rtMask = 0;
		CShadowUtils::SShadowCascades cascades;
		bool bSunShadow = CShadowUtils::SetupShadowsForFog(rtMask, cascades, RenderView());

		uint32 inputFlag = 0;
		inputFlag |= bCloudShadow ? BIT(0) : 0;
		inputFlag |= bVolCloudShadow ? BIT(1) : 0;
		inputFlag |= bSunShadow ? BIT(2) : 0;

		if (m_passVolFogShadowRaycast.InputChanged(inputFlag, CRenderer::CV_r_FogShadowsMode))
		{
			rtMask |= bCloudShadow ? g_HWSR_MaskBit[HWSR_SAMPLE5] : 0;
			rtMask |= bVolCloudShadow ? g_HWSR_MaskBit[HWSR_SAMPLE4] : 0;

			static CCryNameTSCRC TechName0("FogPassVolShadowsInterleavePass");
			static CCryNameTSCRC TechName1("MultiGSMShadowedFog");
			auto& techName = (CRenderer::CV_r_FogShadowsMode == 1) ? TechName1 : TechName0;

			m_passVolFogShadowRaycast.SetTechnique(pShader, techName, rtMask);
			m_passVolFogShadowRaycast.SetRenderTarget(0, CTexture::s_ptexVolFogShadowBuf[0]);
			m_passVolFogShadowRaycast.SetState(GS_NODEPTHTEST);
			m_passVolFogShadowRaycast.SetRequireWorldPos(true);
			m_passVolFogShadowRaycast.SetRequirePerViewConstantBuffer(true);

			m_passVolFogShadowRaycast.SetTexture(0, CTexture::s_ptexZTarget);
			m_passVolFogShadowRaycast.SetTexture(1, m_pTexInterleaveSamplePattern);

			CShadowUtils::SetShadowSamplingContextToRenderPass(m_passVolFogShadowRaycast, 0, 1, 2, 3, 2);
			CShadowUtils::SetShadowCascadesToRenderPass(m_passVolFogShadowRaycast, 4, 3, cascades);

			if (bVolCloudShadow)
			{
				m_passVolFogShadowRaycast.SetTexture(8, CTexture::s_ptexVolCloudShadow);
				m_passVolFogShadowRaycast.SetSampler(4, rd->m_nBilinearBorderSampler);
			}
		}

		m_passVolFogShadowRaycast.BeginConstantUpdate();

		Vec4 volFogShadowRange;
		{
			Vec3 volFogShadowRangeP;
			gEnv->p3DEngine->GetGlobalParameter(E3DPARAM_VOLFOG_SHADOW_RANGE, volFogShadowRangeP);
			volFogShadowRangeP.x = clamp_tpl(volFogShadowRangeP.x, 0.01f, 1.0f);
			volFogShadowRange = Vec4(volFogShadowRangeP.x, 1.0f / volFogShadowRangeP.x, 0, 0);
		}

		static CCryNameR volFogShadowRangeN("volFogShadowRange");
		m_passVolFogShadowRaycast.SetConstant(eHWSC_Pixel, volFogShadowRangeN, volFogShadowRange);

		m_passVolFogShadowRaycast.Execute();
	}

	// horizontal blur
	{
		if (m_passVolFogShadowHBlur.InputChanged())
		{
			static CCryNameTSCRC TechName("FogPassVolShadowsGatherPass");
			m_passVolFogShadowHBlur.SetTechnique(pShader, TechName, 0);
			m_passVolFogShadowHBlur.SetRenderTarget(0, CTexture::s_ptexVolFogShadowBuf[1]);
			m_passVolFogShadowHBlur.SetState(GS_NODEPTHTEST);
			m_passVolFogShadowHBlur.SetRequireWorldPos(true);

			m_passVolFogShadowHBlur.SetTextureSamplerPair(0, CTexture::s_ptexVolFogShadowBuf[0], m_samplerPointClamp);
		}

		m_passVolFogShadowHBlur.BeginConstantUpdate();

		Vec4 sampleOffsets[8];
		const f32 tU = 1.0f / static_cast<f32>(CTexture::s_ptexVolFogShadowBuf[0]->GetWidth());
		for (int32 x = -4, index = 0; x < 4; ++x, ++index)
		{
			sampleOffsets[index] = Vec4(x * tU, 0.0f, 0.0f, 1.0f);
		}
		static CCryNameR volFogShadowBufSampleOffsetsN("volFogShadowBufSampleOffsets");
		m_passVolFogShadowHBlur.SetConstantArray(eHWSC_Pixel, volFogShadowBufSampleOffsetsN, sampleOffsets, 8);

		m_passVolFogShadowHBlur.Execute();
	}

	// vertical blur
	{
		if (m_passVolFogShadowVBlur.InputChanged())
		{
			static CCryNameTSCRC TechName("FogPassVolShadowsGatherPass");
			m_passVolFogShadowVBlur.SetTechnique(pShader, TechName, 0);
			m_passVolFogShadowVBlur.SetRenderTarget(0, CTexture::s_ptexVolFogShadowBuf[0]);
			m_passVolFogShadowVBlur.SetState(GS_NODEPTHTEST);
			m_passVolFogShadowVBlur.SetRequireWorldPos(true);

			m_passVolFogShadowVBlur.SetTextureSamplerPair(0, CTexture::s_ptexVolFogShadowBuf[1], m_samplerPointClamp);
		}

		m_passVolFogShadowVBlur.BeginConstantUpdate();

		Vec4 sampleOffsets[8];
		const f32 tV = 1.0f / static_cast<f32>(CTexture::s_ptexVolFogShadowBuf[1]->GetWidth());
		for (int32 y = -4, index = 0; y < 4; ++y, ++index)
		{
			sampleOffsets[index] = Vec4(0.0f, y * tV, 0.0f, 1.0f);
		}
		static CCryNameR volFogShadowBufSampleOffsetsN("volFogShadowBufSampleOffsets");
		m_passVolFogShadowVBlur.SetConstantArray(eHWSC_Pixel, volFogShadowBufSampleOffsetsN, sampleOffsets, 8);

		m_passVolFogShadowVBlur.Execute();
	}
#endif // #if defined(VOLUMETRIC_FOG_SHADOWS)
}

f32 CFogStage::GetFogCullDistance() const
{
	CD3D9Renderer* const __restrict rd = gcpRendD3D;

	float fogDepth = 0.0f;

	if (CRenderer::CV_r_FogDepthTest != 0.0f)
	{
		if (CRenderer::CV_r_FogDepthTest < 0.0f)
		{
			Vec4 fogColGradColBase(0, 0, 0, 0);
			Vec4 fogColGradColDelta(0, 0, 0, 0);
			CHWShader_D3D::GetFogColorGradientConstants(fogColGradColBase, fogColGradColDelta);
			const Vec4 fogColGradRadial = CHWShader_D3D::GetFogColorGradientRadial();
			const Vec4 volFogParams = CHWShader_D3D::GetVolumetricFogParams();
			const Vec4 volFogRampParams = CHWShader_D3D::GetVolumetricFogRampParams();
			const Vec4 volFogSunDir = CHWShader_D3D::GetVolumetricFogSunDir();

			const float fogColorIntensityBase = MaxChannel(fogColGradColBase);
			const float fogColorIntensityTop = MaxChannel(fogColGradColBase + fogColGradColDelta);
			const float fogColorIntensityRadial = MaxChannel(fogColGradRadial);
			const float fogColorIntensity = max(fogColorIntensityBase, fogColorIntensityTop) + fogColorIntensityRadial;

			const float threshold = -CRenderer::CV_r_FogDepthTest;

			const float atmosphereScale = volFogParams.x;
			const float volFogHeightDensityAtViewer = volFogParams.y;
			const float heightDiffFromBase = volFogParams.z;
			const float expHeightDiffFromBase = volFogParams.w;
			const float finalClamp = 1.0f - volFogSunDir.w;

			SD3DPostEffectsUtils::UpdateFrustumCorners();
			Vec3 camDir = rd->GetRCamera().ViewDir();
			f32 camZFar = rd->GetRCamera().fFar;

			Vec3 lookDir = SD3DPostEffectsUtils::m_vRT;
			if (lookDir.z * atmosphereScale < SD3DPostEffectsUtils::m_vLT.z * atmosphereScale)
				lookDir = SD3DPostEffectsUtils::m_vLT;
			if (lookDir.z * atmosphereScale < SD3DPostEffectsUtils::m_vLB.z * atmosphereScale)
				lookDir = SD3DPostEffectsUtils::m_vLB;
			if (lookDir.z * atmosphereScale < SD3DPostEffectsUtils::m_vRB.z * atmosphereScale)
				lookDir = SD3DPostEffectsUtils::m_vRB;

			lookDir.Normalize();
			const float viewDirAdj = lookDir.Dot(camDir);

			float depth = camZFar * 0.5f;
			float step = depth * 0.5f;
			uint32 numSteps = 16;

			while (numSteps)
			{
				Vec3 cameraToWorldPos = lookDir * depth;

				float fogInt = 1.0f;

				const float t = atmosphereScale * cameraToWorldPos.z;
				const float slopeThreshold = 0.01f;
				if (fabsf(t) > slopeThreshold)
				{
					fogInt *= ((expf_s(t + heightDiffFromBase) - expHeightDiffFromBase) / t);
				}
				else
				{
					fogInt *= expHeightDiffFromBase;
				}

				const float l = depth; // length(cameraToWorldPos);
				const float u = l * volFogHeightDensityAtViewer;
				fogInt = fogInt * u;

				float f = clamp_tpl(expf_s(0.69314719f * -fogInt), 0.0f, 1.0f);

				float r = clamp_tpl(l * volFogRampParams.x + volFogRampParams.y, 0.0f, 1.0f);
				r = r * (2.0f - r);
				r = r * volFogRampParams.z + volFogRampParams.w;

				f = (1.0f - f) * r;
				assert(f >= 0.0f && f <= 1.0f);

				f = min(f, finalClamp);
				f *= fogColorIntensity;

				if (f > threshold)
				{
					depth -= step;
				}
				else
				{
					fogDepth = depth * viewDirAdj;
					depth += step;
				}
				step *= 0.5f;

				--numSteps;
			}
		}
		else
		{
			fogDepth = CRenderer::CV_r_FogDepthTest;
		}
	}

	return fogDepth;
}

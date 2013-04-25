#include <stdio.h>
#include <assert.h>
#include <math.h>
#include "AppConfig.h"
#include "GSH_OpenGL.h"
#include "PtrMacro.h"
#include "Log.h"

//#define _WIREFRAME

GLenum CGSH_OpenGL::g_nativeClampModes[CGSHandler::CLAMP_MODE_MAX] =
{
	GL_REPEAT,
	GL_CLAMP_TO_EDGE,
	GL_REPEAT,
	GL_REPEAT
};

unsigned int CGSH_OpenGL::g_shaderClampModes[CGSHandler::CLAMP_MODE_MAX] =
{
	TEXTURE_CLAMP_MODE_STD,
	TEXTURE_CLAMP_MODE_STD,
	TEXTURE_CLAMP_MODE_REGION_CLAMP,
	TEXTURE_CLAMP_MODE_REGION_REPEAT
};

CGSH_OpenGL::CGSH_OpenGL() 
: m_pCvtBuffer(nullptr)
{
	CAppConfig::GetInstance().RegisterPreferenceBoolean(PREF_CGSH_OPENGL_LINEASQUADS, false);
	CAppConfig::GetInstance().RegisterPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES, false);
	CAppConfig::GetInstance().RegisterPreferenceBoolean(PREF_CGSH_OPENGL_FIXSMALLZVALUES, false);

	LoadSettings();

	m_pCvtBuffer = new uint8[CVTBUFFERSIZE];

	memset(&m_renderState, 0, sizeof(m_renderState));
}

CGSH_OpenGL::~CGSH_OpenGL()
{
	delete [] m_pCvtBuffer;
}

void CGSH_OpenGL::InitializeImpl()
{
	InitializeRC();

	m_nVtxCount = 0;
	memset(m_clampMin, 0, sizeof(m_clampMin));
	memset(m_clampMax, 0, sizeof(m_clampMax));

	for(unsigned int i = 0; i < MAX_TEXTURE_CACHE; i++)
	{
		m_textureCache.push_back(TexturePtr(new CTexture()));
	}

	for(unsigned int i = 0; i < MAX_PALETTE_CACHE; i++)
	{
		m_paletteCache.push_back(PalettePtr(new CPalette()));
	}

	m_nMaxZ = 32768.0;
	m_renderState.isValid = false;
}

void CGSH_OpenGL::ReleaseImpl()
{
	TexCache_Flush();
	PalCache_Flush();
	m_textureCache.clear();
	m_paletteCache.clear();
	m_shaderInfos.clear();
	m_framebuffers.clear();
}

void CGSH_OpenGL::ResetImpl()
{
	TexCache_Flush();
	PalCache_Flush();
	m_framebuffers.clear();
}

void CGSH_OpenGL::FlipImpl()
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0, 0, 0, 0);
	glViewport(0, 0, m_presentationParams.windowWidth, m_presentationParams.windowHeight);
	glClear(GL_COLOR_BUFFER_BIT);

	unsigned int sourceWidth = GetCrtWidth();
	unsigned int sourceHeight = GetCrtHeight();
	switch(m_presentationParams.mode)
	{
	case PRESENTATION_MODE_FILL:
		glViewport(0, 0, m_presentationParams.windowWidth, m_presentationParams.windowHeight);
		break;
	case PRESENTATION_MODE_FIT:
		{
			int viewportWidth[2];
			int viewportHeight[2];
			{
				viewportWidth[0] = m_presentationParams.windowWidth;
				viewportHeight[0] = (sourceWidth != 0) ? (m_presentationParams.windowWidth * sourceHeight) / sourceWidth : 0;
			}
			{
				viewportWidth[1] = (sourceHeight != 0) ? (m_presentationParams.windowHeight * sourceWidth) / sourceHeight : 0;
				viewportHeight[1] = m_presentationParams.windowHeight;
			}
			int selectedViewport = 0;
			if(
			   (viewportWidth[0] > static_cast<int>(m_presentationParams.windowWidth)) ||
			   (viewportHeight[0] > static_cast<int>(m_presentationParams.windowHeight))
			   )
			{
				selectedViewport = 1;
				assert(
					   viewportWidth[1] <= static_cast<int>(m_presentationParams.windowWidth) &&
					   viewportHeight[1] <= static_cast<int>(m_presentationParams.windowHeight));
			}
			int offsetX = static_cast<int>(m_presentationParams.windowWidth - viewportWidth[selectedViewport]) / 2;
			int offsetY = static_cast<int>(m_presentationParams.windowHeight - viewportHeight[selectedViewport]) / 2;
			glViewport(offsetX, offsetY, viewportWidth[selectedViewport], viewportHeight[selectedViewport]);
		}
		break;
	case PRESENTATION_MODE_ORIGINAL:
		{
			int offsetX = static_cast<int>(m_presentationParams.windowWidth - sourceWidth) / 2;
			int offsetY = static_cast<int>(m_presentationParams.windowHeight - sourceHeight) / 2;
			glViewport(offsetX, offsetY, sourceWidth, sourceHeight);
		}
		break;
	}

	DISPLAY d;
	DISPFB fb;
	{
		std::lock_guard<std::recursive_mutex> registerMutexLock(m_registerMutex);
		unsigned int readCircuit = GetCurrentReadCircuit();
//		unsigned int readCircuit = 1;
		switch(readCircuit)
		{
		case 0:
			d <<= m_nDISPLAY1.value.q;
			fb <<= m_nDISPFB1.value.q;
			break;
		case 1:
			d <<= m_nDISPLAY2.value.q;
			fb <<= m_nDISPFB2.value.q;
			break;
		}
	}

	unsigned int dispWidth = (d.nW + 1) / (d.nMagX + 1);
	unsigned int dispHeight = (d.nH + 1);

	FramebufferPtr framebuffer;
	for(auto framebufferIterator(std::begin(m_framebuffers));
		framebufferIterator != std::end(m_framebuffers); framebufferIterator++)
	{
		const auto& candidateFramebuffer = *framebufferIterator;
		if(candidateFramebuffer->m_basePtr == fb.GetBufPtr())
		{
			//We have a winner
			framebuffer = candidateFramebuffer;
			break;
		}
	}

	if(!framebuffer && (fb.GetBufWidth() != 0))
	{
		framebuffer = FramebufferPtr(new CFramebuffer(fb.GetBufPtr(), fb.GetBufWidth(), 1024, fb.nPSM));
		m_framebuffers.push_back(framebuffer);
	}

	if(framebuffer)
	{
		float u0 = 0;
		float u1 = static_cast<float>(dispWidth) / static_cast<float>(framebuffer->m_width);

		float v0 = 0;
		float v1 = static_cast<float>(dispHeight) / static_cast<float>(framebuffer->m_height);

		glUseProgram(0);

		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();

		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_ALPHA_TEST);
		glColor4f(1, 1, 1, 1);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, 0);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, framebuffer->m_texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();

		glBegin(GL_QUADS);
		{
			glTexCoord2f(u1, v0);
			glVertex2f(1, 1);

			glTexCoord2f(u1, v1);
			glVertex2f(1, -1);

			glTexCoord2f(u0, v1);
			glVertex2f(-1, -1);

			glTexCoord2f(u0, v0);
			glVertex2f(-1, 1);
		}
		glEnd();

		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
	}

	m_renderState.isValid = false;

	assert(glGetError() == GL_NO_ERROR);

	static bool g_dumpFramebuffers = false;
	if(g_dumpFramebuffers)
	{
		for(auto framebufferIterator(std::begin(m_framebuffers));
			framebufferIterator != std::end(m_framebuffers); framebufferIterator++)
		{
			const auto& framebuffer(*framebufferIterator);
			glBindTexture(GL_TEXTURE_2D, framebuffer->m_texture);
			DumpTexture(framebuffer->m_width, framebuffer->m_height, framebuffer->m_basePtr);
		}
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	PresentBackbuffer();
	CGSHandler::FlipImpl();
}

void CGSH_OpenGL::LoadState(Framework::CZipArchiveReader& archive)
{
	CGSHandler::LoadState(archive);

	m_mailBox.SendCall(std::bind(&CGSH_OpenGL::TexCache_InvalidateTextures, this, 0, RAMSIZE));
}

void CGSH_OpenGL::LoadSettings()
{
	CGSHandler::LoadSettings();

	m_nLinesAsQuads				= CAppConfig::GetInstance().GetPreferenceBoolean(PREF_CGSH_OPENGL_LINEASQUADS);
	m_nForceBilinearTextures	= CAppConfig::GetInstance().GetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES);
	m_fixSmallZValues			= CAppConfig::GetInstance().GetPreferenceBoolean(PREF_CGSH_OPENGL_FIXSMALLZVALUES);
}

void CGSH_OpenGL::InitializeRC()
{
#ifdef WIN32
	glewInit();
#endif

	//Initialize basic stuff
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(0.0f);
	glEnable(GL_TEXTURE_2D);

	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

	//Initialize fog
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 0.0f);
	glFogf(GL_FOG_END, 1.0f);
	glHint(GL_FOG_HINT, GL_NICEST);
	glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FOG_COORDINATE_EXT);

	VerifyRGBA5551Support();
	if(!m_nIsRGBA5551Supported)
	{
		m_pTexUploader_Psm16 = &CGSH_OpenGL::TexUploader_Psm16_Cvt;
	}
	else
	{
		m_pTexUploader_Psm16 = &CGSH_OpenGL::TexUploader_Psm16_Hw;
	}

#ifdef _WIREFRAME
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	glDisable(GL_TEXTURE_2D);
#endif

	PresentBackbuffer();
}

void CGSH_OpenGL::VerifyRGBA5551Support()
{
	unsigned int nTexture = 0;

	m_nIsRGBA5551Supported = true;

	glGenTextures(1, &nTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 32, 32, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, m_pRAM);
	if(glGetError() == GL_INVALID_ENUM)
	{
		m_nIsRGBA5551Supported = false;
	}
	glDeleteTextures(1, &nTexture);
}

void CGSH_OpenGL::LinearZOrtho(float nLeft, float nRight, float nBottom, float nTop)
{
	float nMatrix[16];

	nMatrix[ 0] = 2.0f / (nRight - nLeft);
	nMatrix[ 1] = 0;
	nMatrix[ 2] = 0;
	nMatrix[ 3] = 0;

	nMatrix[ 4] = 0;
	nMatrix[ 5] = 2.0f / (nTop - nBottom);
	nMatrix[ 6] = 0;
	nMatrix[ 7] = 0;

	nMatrix[ 8] = 0;
	nMatrix[ 9] = 0;
	nMatrix[10] = 1;
	nMatrix[11] = 0;

	nMatrix[12] = - (nRight + nLeft) / (nRight - nLeft);
	nMatrix[13] = - (nTop + nBottom) / (nTop - nBottom);
	nMatrix[14] = 0;
	nMatrix[15] = 1;

	glMultMatrixf(nMatrix);
}

unsigned int CGSH_OpenGL::GetCurrentReadCircuit()
{
//	assert((m_nPMODE & 0x3) != 0x03);
	if(m_nPMODE & 0x1) return 0;
	if(m_nPMODE & 0x2) return 1;
	//Getting here is bad
	return 0;
}

uint32 CGSH_OpenGL::RGBA16ToRGBA32(uint16 nColor)
{
	return (nColor & 0x8000 ? 0xFF000000 : 0) | ((nColor & 0x7C00) << 9) | ((nColor & 0x03E0) << 6) | ((nColor & 0x001F) << 3);
}

uint8 CGSH_OpenGL::MulBy2Clamp(uint8 nValue)
{
	return (nValue > 0x7F) ? 0xFF : (nValue << 1);
}

float CGSH_OpenGL::GetZ(float nZ)
{
	if(nZ == 0)
	{
		return -1;
	}
	
	if(m_fixSmallZValues && (nZ < 256))
	{
		//The number is small, so scale to a smaller ratio (65536)
		return (nZ - 32768.0f) / 32768.0f;
	}
	else
	{
		nZ -= m_nMaxZ;
		if(nZ > m_nMaxZ) return 1.0;
		if(nZ < -m_nMaxZ) return -1.0;
		return nZ / m_nMaxZ;
	}
}

unsigned int CGSH_OpenGL::GetNextPowerOf2(unsigned int nNumber)
{
	return 1 << ((int)(log((double)(nNumber - 1)) / log(2.0)) + 1);
}

/////////////////////////////////////////////////////////////
// Context Unpacking
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::SetRenderingContext(unsigned int nContext)
{
	SHADERCAPS shaderCaps;
	memset(&shaderCaps, 0, sizeof(SHADERCAPS));

	uint64 testReg = m_nReg[GS_REG_TEST_1 + nContext];
	uint64 frameReg = m_nReg[GS_REG_FRAME_1 + nContext];
	uint64 alphaReg = m_nReg[GS_REG_ALPHA_1 + nContext];
	uint64 zbufReg = m_nReg[GS_REG_ZBUF_1 + nContext];

	if(!m_renderState.isValid ||
		(m_renderState.alphaReg != alphaReg))
	{
		SetupBlendingFunction(alphaReg);
		m_renderState.alphaReg = alphaReg;
	}

	if(!m_renderState.isValid ||
		(m_renderState.testReg != testReg))
	{
		SetupTestFunctions(testReg);
		m_renderState.testReg = testReg;
	}

	if(!m_renderState.isValid ||
		(m_renderState.zbufReg != zbufReg))
	{
		SetupDepthBuffer(zbufReg);
		m_renderState.zbufReg = zbufReg;
	}

	if(!m_renderState.isValid ||
		(m_renderState.frameReg != frameReg))
	{
		SetupFramebuffer(frameReg);
		m_renderState.frameReg = frameReg;
	}

	SetupTexture(shaderCaps, m_nReg[GS_REG_TEX0_1 + nContext], m_nReg[GS_REG_TEX1_1 + nContext], m_nReg[GS_REG_CLAMP_1 + nContext]);

	if(m_PrimitiveMode.nFog)
	{
		shaderCaps.hasFog = 1;
	}

	XYOFFSET offset;
	offset <<= m_nReg[GS_REG_XYOFFSET_1 + nContext];
	m_nPrimOfsX = offset.GetX();
	m_nPrimOfsY = offset.GetY();
	
	auto shaderInfoIterator = m_shaderInfos.find(static_cast<uint32>(shaderCaps));
	if(shaderInfoIterator == m_shaderInfos.end())
	{
		auto shader = GenerateShader(shaderCaps);
		SHADERINFO shaderInfo;
		shaderInfo.shader = shader;

		shaderInfo.textureUniform		= glGetUniformLocation(*shader, "g_texture");
		shaderInfo.paletteUniform		= glGetUniformLocation(*shader, "g_palette");
		shaderInfo.textureSizeUniform	= glGetUniformLocation(*shader, "g_textureSize");
		shaderInfo.texelSizeUniform		= glGetUniformLocation(*shader, "g_texelSize");
		shaderInfo.clampMinUniform		= glGetUniformLocation(*shader, "g_clampMin");
		shaderInfo.clampMaxUniform		= glGetUniformLocation(*shader, "g_clampMax");

		m_shaderInfos.insert(ShaderInfoMap::value_type(static_cast<uint32>(shaderCaps), shaderInfo));
		shaderInfoIterator = m_shaderInfos.find(static_cast<uint32>(shaderCaps));
	}

	const auto& shaderInfo = shaderInfoIterator->second;

	GLuint shaderHandle = *shaderInfo.shader;
	if(!m_renderState.isValid ||
		(m_renderState.shaderHandle != shaderHandle))
	{
		glUseProgram(shaderHandle);

		if(shaderInfo.textureUniform != -1)
		{
			glUniform1i(shaderInfo.textureUniform, 0);
		}

		if(shaderInfo.paletteUniform != -1)
		{
			glUniform1i(shaderInfo.paletteUniform, 1);
		}

		m_renderState.shaderHandle = shaderHandle;
	}

	TEX0 tex0;
	tex0 <<= m_nReg[GS_REG_TEX0_1 + nContext];

	if(shaderInfo.textureSizeUniform != -1)
	{
		glUniform2f(shaderInfo.textureSizeUniform, 
			static_cast<float>(tex0.GetWidth()), static_cast<float>(tex0.GetHeight()));
	}

	if(shaderInfo.texelSizeUniform != -1)
	{
		glUniform2f(shaderInfo.texelSizeUniform, 
			1.0f / static_cast<float>(tex0.GetWidth()), 1.0f / static_cast<float>(tex0.GetHeight()));
	}

	if(shaderInfo.clampMinUniform != -1)
	{
		glUniform2f(shaderInfo.clampMinUniform,
			static_cast<float>(m_clampMin[0]), static_cast<float>(m_clampMin[1]));
	}

	if(shaderInfo.clampMaxUniform != -1)
	{
		glUniform2f(shaderInfo.clampMaxUniform,
			static_cast<float>(m_clampMax[0]), static_cast<float>(m_clampMax[1]));
	}

	assert(glGetError() == GL_NO_ERROR);

	m_renderState.isValid = true;
}

void CGSH_OpenGL::SetupBlendingFunction(uint64 nData)
{
	if(nData == 0) return;

	int nFunction = GL_FUNC_ADD_EXT;

	ALPHA alpha;
	alpha <<= nData;

	if((alpha.nA == 0) && (alpha.nB == 0) && (alpha.nC == 0) && (alpha.nD == 0))
	{
		glBlendFunc(GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 0) && (alpha.nD == 1))
	{
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 2) && (alpha.nD == 1) && (alpha.nFix == 0x80))
	{
		glBlendFunc(GL_ONE, GL_ONE);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 2) && (alpha.nD == 1) && (alpha.nFix == 0x80))
	{
		glBlendFunc(GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 2) && (alpha.nD == 1))
	{
		//Source alpha value is implied in the formula
		//As = FIX / 0x80
		if(glBlendColorEXT != NULL)
		{
			glBlendColorEXT(0.0f, 0.0f, 0.0f, (float)alpha.nFix / 128.0f);
			glBlendFunc(GL_CONSTANT_ALPHA_EXT, GL_ONE_MINUS_CONSTANT_ALPHA_EXT);
		}
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 2) && (alpha.nD == 1))
	{
		//Cs * FIX + Cd
		if(glBlendColor != NULL)
		{
			glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
			glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
		}
	}
	else if((alpha.nA == 1) && (alpha.nB == 0) && (alpha.nC == 2) && (alpha.nD == 2))
	{
		nFunction = GL_FUNC_REVERSE_SUBTRACT_EXT;
		if(glBlendColorEXT != NULL)
		{
			glBlendColorEXT(0.0f, 0.0f, 0.0f, (float)alpha.nFix / 128.0f);
			glBlendFunc(GL_CONSTANT_ALPHA_EXT, GL_CONSTANT_ALPHA_EXT);
		}
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 1))
	{
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 2))
	{
		//Cs * As
		glBlendFunc(GL_SRC_ALPHA, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 1) && (alpha.nD == 1))
	{
		//Cs * Ad + Cd * (1 - Ad)
		glBlendFunc(GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 1) && (alpha.nD == 1))
	{
		//Cs * Ad + Cd
		glBlendFunc(GL_DST_ALPHA, GL_ONE);
	}
//	else if((alpha.nA == 1) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 1))
//	{
//		//Cd * As + Cd
//		//Implemented as Cd * As
//		glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
//	}
	else if((alpha.nA == 1) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 0))
	{
		//Cd * As + Cs
		glBlendFunc(GL_ONE, GL_SRC_ALPHA);
	}
	else if((alpha.nA == 1) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 2))
	{
		//Cd * As
		//glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
		//REMOVE
		glBlendFunc(GL_ZERO, GL_ONE);
		//REMOVE
	}
	else if((alpha.nA == 2) && (alpha.nB == 0) && (alpha.nC == 0) && (alpha.nD == 1))
	{
		nFunction = GL_FUNC_REVERSE_SUBTRACT_EXT;
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	}
	else
	{
#ifdef _DEBUG
		printf("GSH_OpenGL: Unknown color blending formula.\r\n");
#endif
	}

	if(glBlendEquationEXT != NULL)
	{
		glBlendEquationEXT(nFunction);
	}
}

void CGSH_OpenGL::SetupTestFunctions(uint64 nData)
{
	TEST tst;
	tst <<= nData;

	if(tst.nAlphaEnabled)
	{
		unsigned int nFunc = GL_NEVER;
		switch(tst.nAlphaMethod)
		{
		case 0:
			nFunc = GL_NEVER;
			break;
		case 1:
			nFunc = GL_ALWAYS;
			break;
		case 2:
			nFunc = GL_LESS;
			break;
		case 4:
			nFunc = GL_EQUAL;
			break;
		case 5:
			nFunc = GL_GEQUAL;
			break;
		case 6:
			nFunc = GL_GREATER;
			break;
		case 7:
			nFunc = GL_NOTEQUAL;
			break;
		default:
			assert(0);
			break;
		}

		//Special trick used by Castlevania: Yami no Juin
		//Always fail alpha testing but write RGBA if it fails
		if(tst.nAlphaMethod == 0 && tst.nAlphaFail == 1)
		{
			glDisable(GL_ALPHA_TEST);
		}
		else
		{
			float nValue = (float)tst.nAlphaRef / 255.0f;
			glAlphaFunc(nFunc, nValue);

			glEnable(GL_ALPHA_TEST);
		}
	}
	else
	{
		glDisable(GL_ALPHA_TEST);
	}

	if(tst.nDepthEnabled)
	{
		unsigned int nFunc = GL_NEVER;

		switch(tst.nDepthMethod)
		{
		case 0:
			nFunc = GL_NEVER;
			break;
		case 1:
			nFunc = GL_ALWAYS;
			break;
		case 2:
			nFunc = GL_GEQUAL;
			break;
		case 3:
			nFunc = GL_GREATER;
			break;
		}

		glDepthFunc(nFunc);

		glEnable(GL_DEPTH_TEST);
	}
	else
	{
		glDisable(GL_DEPTH_TEST);
	}
}

void CGSH_OpenGL::SetupDepthBuffer(uint64 nData)
{
	ZBUF zbuf;
	zbuf <<= nData;

	switch(GetPsmPixelSize(zbuf.nPsm))
	{
	case 16:
		m_nMaxZ = 32768.0f;
		break;
	case 24:
		m_nMaxZ = 8388608.0f;
		break;
	default:
	case 32:
		m_nMaxZ = 2147483647.0f;
		break;
	}

	glDepthMask(zbuf.nMask ? GL_FALSE : GL_TRUE);
}

void CGSH_OpenGL::SetupFramebuffer(uint64 frameReg)
{
	if(frameReg == 0) return;

	FRAME frame;
	frame <<= frameReg;

	bool r = (frame.nMask & 0x000000FF) == 0;
	bool g = (frame.nMask & 0x0000FF00) == 0;
	bool b = (frame.nMask & 0x00FF0000) == 0;
	bool a = (frame.nMask & 0xFF000000) == 0;
	glColorMask(r, g, b, a);

	//Look for a framebuffer that matches the specified information
	FramebufferPtr framebuffer;
	for(auto framebufferIterator(std::begin(m_framebuffers));
		framebufferIterator != std::end(m_framebuffers); framebufferIterator++)
	{
		const auto& candidateFramebuffer = *framebufferIterator;
		if(
			(candidateFramebuffer->m_basePtr == frame.GetBasePtr()) &&
			(candidateFramebuffer->m_width == frame.GetWidth())
			)
		{
			framebuffer = candidateFramebuffer;
			break;
		}
	}

	if(!framebuffer)
	{
		framebuffer = FramebufferPtr(new CFramebuffer(frame.GetBasePtr(), frame.GetWidth(), 1024, frame.nPsm));
		m_framebuffers.push_back(framebuffer);
	}

	//Any framebuffer selected at this point can be used as a texture
	framebuffer->m_canBeUsedAsTexture = true;

	bool halfHeight = GetCrtIsInterlaced() && GetCrtIsFrameMode();

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);
	glViewport(0, 0, framebuffer->m_width, framebuffer->m_height);
	glScissor(0, 0, framebuffer->m_width, framebuffer->m_height);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();

	float projWidth = static_cast<float>(framebuffer->m_width);
	float projHeight = static_cast<float>(halfHeight ? (framebuffer->m_height / 2) : framebuffer->m_height);
	LinearZOrtho(0, projWidth, 0, projHeight);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

void CGSH_OpenGL::SetupTexture(SHADERCAPS& shaderCaps, uint64 nTex0, uint64 nTex1, uint64 nClamp)
{
	if(nTex0 == 0 || m_PrimitiveMode.nTexture == 0)
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, 0);
		return;
	}

	shaderCaps.texSourceMode = TEXTURE_SOURCE_MODE_STD;

	TEX0 tex0;
	TEX1 tex1;
	CLAMP clamp;

	tex0 <<= nTex0;
	tex1 <<= nTex1;
	clamp <<= nClamp;

	m_nTexWidth = tex0.GetWidth();
	m_nTexHeight = tex0.GetHeight();

	glActiveTexture(GL_TEXTURE0);
	PrepareTexture(tex0);

	int nMagFilter, nMinFilter;

	//Setup sampling modes
	if(tex1.nMagFilter == 0)
	{
		nMagFilter = GL_NEAREST;
	}
	else
	{
		nMagFilter = GL_LINEAR;
	}

	switch(tex1.nMinFilter)
	{
	case 0:
		nMinFilter = GL_NEAREST;
		break;
	case 1:
		nMinFilter = GL_LINEAR;
		break;
	case 5:
		nMinFilter = GL_LINEAR_MIPMAP_LINEAR;
		break;
	default:
		assert(0);
		break;
	}

	if(m_nForceBilinearTextures)
	{
		nMagFilter = GL_LINEAR;
		nMinFilter = GL_LINEAR;
	}

	GLenum nWrapS = g_nativeClampModes[clamp.nWMS];
	GLenum nWrapT = g_nativeClampModes[clamp.nWMT];

	if((clamp.nWMS > CLAMP_MODE_CLAMP) || (clamp.nWMT > CLAMP_MODE_CLAMP))
	{
		unsigned int clampMode[2];

		clampMode[0] = g_shaderClampModes[clamp.nWMS];
		clampMode[1] = g_shaderClampModes[clamp.nWMT];

		m_clampMin[0] = clamp.GetMinU();
		m_clampMin[1] = clamp.GetMinV();

		m_clampMax[0] = clamp.GetMaxU();
		m_clampMax[1] = clamp.GetMaxV();

		for(unsigned int i = 0; i < 2; i++)
		{
			if(clampMode[i] == TEXTURE_CLAMP_MODE_REGION_REPEAT)
			{
				//Check if we can transform in something more elegant
				for(unsigned int j = 1; j < 0x3FF; j = ((j << 1) | 1))
				{
					if(m_clampMin[i] < j) break;
					if(m_clampMin[i] != j) continue;

					if((m_clampMin[i] & m_clampMax[i]) != 0) break;

					m_clampMin[i]++;
					clampMode[i] = TEXTURE_CLAMP_MODE_REGION_REPEAT_SIMPLE;
				}
			}
		}

		shaderCaps.texClampS = clampMode[0];
		shaderCaps.texClampT = clampMode[1];
	}

	if(IsPsmIDTEX(tex0.nPsm) && (nMinFilter != GL_NEAREST || nMagFilter != GL_NEAREST))
	{
		//We'll need to filter the texture manually
		shaderCaps.texBilinearFilter = 1;
		nMinFilter = GL_NEAREST;
		nMagFilter = GL_NEAREST;
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nMagFilter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nMinFilter);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, nWrapS);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, nWrapT);

	if(IsPsmIDTEX(tex0.nPsm))
	{
		glActiveTexture(GL_TEXTURE1);
		PreparePalette(tex0);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		shaderCaps.texSourceMode = IsPsmIDTEX4(tex0.nPsm) ? TEXTURE_SOURCE_MODE_IDX4 : TEXTURE_SOURCE_MODE_IDX8;
	}

	shaderCaps.texFunction = tex0.nFunction;
}

void CGSH_OpenGL::SetupFogColor()
{
	float nColor[4];

	FOGCOL color;
	color <<= m_nReg[GS_REG_FOGCOL];
	nColor[0] = static_cast<float>(color.nFCR) / 255.0f;
	nColor[1] = static_cast<float>(color.nFCG) / 255.0f;
	nColor[2] = static_cast<float>(color.nFCB) / 255.0f;
	nColor[3] = 0.0f;

	glFogfv(GL_FOG_COLOR, nColor);
}

/////////////////////////////////////////////////////////////
// Individual Primitives Implementations
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::Prim_Point()
{
	XYZ xyz;
	xyz <<= m_VtxBuffer[0].nPosition;

	float nX = xyz.GetX();	float nY = xyz.GetY();	float nZ = xyz.GetZ();

	nX -= m_nPrimOfsX;
	nY -= m_nPrimOfsY;

	if(!m_PrimitiveMode.nUseUV && !m_PrimitiveMode.nTexture)
	{
		RGBAQ rgbaq;
		rgbaq <<= m_VtxBuffer[0].nRGBAQ;

		glBegin(GL_QUADS);

			glColor4ub(rgbaq.nR, rgbaq.nG, rgbaq.nB, rgbaq.nA);

			glVertex2f(nX + 0, nY + 0);
			glVertex2f(nX + 1, nY + 0);
			glVertex2f(nX + 1, nY + 1);
			glVertex2f(nX + 0, nY + 1);

		glEnd();
	}
	else
	{
		//Yay for textured points!
		assert(0);
	}
}

void CGSH_OpenGL::Prim_Line()
{
	XYZ xyz[2];
	xyz[0] <<= m_VtxBuffer[1].nPosition;
	xyz[1] <<= m_VtxBuffer[0].nPosition;

	float nX1 = xyz[0].GetX();	float nY1 = xyz[0].GetY();	float nZ1 = xyz[0].GetZ();
	float nX2 = xyz[1].GetX();	float nY2 = xyz[1].GetY();	float nZ2 = xyz[1].GetZ();

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;

	if(m_PrimitiveMode.nAntiAliasing)
	{
		glEnable(GL_BLEND);
	}

	if(!m_PrimitiveMode.nUseUV && !m_PrimitiveMode.nTexture)
	{
		RGBAQ rgbaq[2];
		rgbaq[0] <<= m_VtxBuffer[1].nRGBAQ;
		rgbaq[1] <<= m_VtxBuffer[0].nRGBAQ;

		if(m_nLinesAsQuads)
		{
			glBegin(GL_QUADS);

				glColor4ub(rgbaq[0].nR, rgbaq[0].nG, rgbaq[0].nB, rgbaq[0].nA);
		
				glVertex2f(nX1 + 0, nY1 + 0);
				glVertex2f(nX1 + 1, nY1 + 1);

				glColor4ub(rgbaq[1].nR, rgbaq[1].nG, rgbaq[1].nB, rgbaq[1].nA);

				glVertex2f(nX2 + 1, nY2 + 1);
				glVertex2f(nX2 + 0, nY2 + 0);

			glEnd();
		}
		else
		{
			glBegin(GL_LINES);

				glColor4ub(rgbaq[0].nR, rgbaq[0].nG, rgbaq[0].nB, rgbaq[0].nA);
				glVertex2f(nX1, nY1);

				glColor4ub(rgbaq[1].nR, rgbaq[1].nG, rgbaq[1].nB, rgbaq[1].nA);
				glVertex2f(nX2, nY2);

			glEnd();
		}
	}
	else
	{
		//Yay for textured lines!
		assert(0);
	}

	if(m_PrimitiveMode.nAntiAliasing)
	{
		glDisable(GL_BLEND);
	}
}

void CGSH_OpenGL::Prim_Triangle()
{
	float nF1, nF2, nF3;

	RGBAQ rgbaq[3];

	XYZ vertex[3];
	vertex[0] <<= m_VtxBuffer[2].nPosition;
	vertex[1] <<= m_VtxBuffer[1].nPosition;
	vertex[2] <<= m_VtxBuffer[0].nPosition;

	float nX1 = vertex[0].GetX(), nX2 = vertex[1].GetX(), nX3 = vertex[2].GetX();
	float nY1 = vertex[0].GetY(), nY2 = vertex[1].GetY(), nY3 = vertex[2].GetY();
	float nZ1 = vertex[0].GetZ(), nZ2 = vertex[1].GetZ(), nZ3 = vertex[2].GetZ();

	rgbaq[0] <<= m_VtxBuffer[2].nRGBAQ;
	rgbaq[1] <<= m_VtxBuffer[1].nRGBAQ;
	rgbaq[2] <<= m_VtxBuffer[0].nRGBAQ;

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;
	nX3 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;
	nY3 -= m_nPrimOfsY;

	nZ1 = GetZ(nZ1);
	nZ2 = GetZ(nZ2);
	nZ3 = GetZ(nZ3);

	if(m_PrimitiveMode.nShading)
	{
		glShadeModel(GL_SMOOTH);
	}
	else
	{
		glShadeModel(GL_FLAT);
	}

	if(m_PrimitiveMode.nAlpha)
	{
		glEnable(GL_BLEND);
	}

	if(m_PrimitiveMode.nFog)
	{
		nF1 = (float)(0xFF - m_VtxBuffer[2].nFog) / 255.0f;
		nF2 = (float)(0xFF - m_VtxBuffer[1].nFog) / 255.0f;
		nF3 = (float)(0xFF - m_VtxBuffer[0].nFog) / 255.0f;
	}
	else
	{
		nF1 = nF2 = nF3 = 0.0;
	}

	if(m_PrimitiveMode.nTexture)
	{
		//Textured triangle

		if(m_PrimitiveMode.nUseUV)
		{
			UV uv[3];
			uv[0] <<= m_VtxBuffer[2].nUV;
			uv[1] <<= m_VtxBuffer[1].nUV;
			uv[2] <<= m_VtxBuffer[0].nUV;

			float nU1 = uv[0].GetU() / static_cast<float>(m_nTexWidth);
			float nU2 = uv[1].GetU() / static_cast<float>(m_nTexWidth);
			float nU3 = uv[2].GetU() / static_cast<float>(m_nTexWidth);

			float nV1 = uv[0].GetV() / static_cast<float>(m_nTexHeight);
			float nV2 = uv[1].GetV() / static_cast<float>(m_nTexHeight);
			float nV3 = uv[2].GetV() / static_cast<float>(m_nTexHeight);

			glBegin(GL_TRIANGLES);
			{
				glColor4ub(MulBy2Clamp(rgbaq[0].nR), MulBy2Clamp(rgbaq[0].nG), MulBy2Clamp(rgbaq[0].nB), MulBy2Clamp(rgbaq[0].nA));
				glTexCoord2f(nU1, nV1);
				glVertex3f(nX1, nY1, nZ1);

				glColor4ub(MulBy2Clamp(rgbaq[1].nR), MulBy2Clamp(rgbaq[1].nG), MulBy2Clamp(rgbaq[1].nB), MulBy2Clamp(rgbaq[1].nA));
				glTexCoord2f(nU2, nV2);
				glVertex3f(nX2, nY2, nZ2);

				glColor4ub(MulBy2Clamp(rgbaq[2].nR), MulBy2Clamp(rgbaq[2].nG), MulBy2Clamp(rgbaq[2].nB), MulBy2Clamp(rgbaq[2].nA));
				glTexCoord2f(nU3, nV3);
				glVertex3f(nX3, nY3, nZ3);
			}
			glEnd();
		}
		else
		{
			ST st[3];
			st[0] <<= m_VtxBuffer[2].nST;
			st[1] <<= m_VtxBuffer[1].nST;
			st[2] <<= m_VtxBuffer[0].nST;

			float nS1 = st[0].nS, nS2 = st[1].nS, nS3 = st[2].nS;
			float nT1 = st[0].nT, nT2 = st[1].nT, nT3 = st[2].nT;

			bool isQ0Neg = (rgbaq[0].nQ < 0);
			bool isQ1Neg = (rgbaq[1].nQ < 0);
			bool isQ2Neg = (rgbaq[2].nQ < 0);

			assert(isQ0Neg == isQ1Neg);
			assert(isQ1Neg == isQ2Neg);

			glBegin(GL_TRIANGLES);
			{
				glColor4ub(MulBy2Clamp(rgbaq[0].nR), MulBy2Clamp(rgbaq[0].nG), MulBy2Clamp(rgbaq[0].nB), MulBy2Clamp(rgbaq[0].nA));
				glTexCoord4f(nS1, nT1, 0, rgbaq[0].nQ);
				if(glFogCoordfEXT) glFogCoordfEXT(nF1);
				glVertex3f(nX1, nY1, nZ1);

				glColor4ub(MulBy2Clamp(rgbaq[1].nR), MulBy2Clamp(rgbaq[1].nG), MulBy2Clamp(rgbaq[1].nB), MulBy2Clamp(rgbaq[1].nA));
				glTexCoord4f(nS2, nT2, 0, rgbaq[1].nQ);
				if(glFogCoordfEXT) glFogCoordfEXT(nF2);
				glVertex3f(nX2, nY2, nZ2);

				glColor4ub(MulBy2Clamp(rgbaq[2].nR), MulBy2Clamp(rgbaq[2].nG), MulBy2Clamp(rgbaq[2].nB), MulBy2Clamp(rgbaq[2].nA));
				glTexCoord4f(nS3, nT3, 0, rgbaq[2].nQ);
				if(glFogCoordfEXT) glFogCoordfEXT(nF3);
				glVertex3f(nX3, nY3, nZ3);
			}
			glEnd();
		}
	}
	else
	{
		//Non Textured Triangle
		glBegin(GL_TRIANGLES);
			
			glColor4ub(rgbaq[0].nR, rgbaq[0].nG, rgbaq[0].nB, MulBy2Clamp(rgbaq[0].nA));
			glVertex3f(nX1, nY1, nZ1);

			glColor4ub(rgbaq[1].nR, rgbaq[1].nG, rgbaq[1].nB, MulBy2Clamp(rgbaq[1].nA));
			glVertex3f(nX2, nY2, nZ2);

			glColor4ub(rgbaq[2].nR, rgbaq[2].nG, rgbaq[2].nB, MulBy2Clamp(rgbaq[2].nA));
			glVertex3f(nX3, nY3, nZ3);

		glEnd();
	}

	if(m_PrimitiveMode.nAlpha)
	{
		glDisable(GL_BLEND);
	}
}

void CGSH_OpenGL::Prim_Sprite()
{
	RGBAQ rgbaq[2];

	XYZ xyz[2];
	xyz[0] <<= m_VtxBuffer[1].nPosition;
	xyz[1] <<= m_VtxBuffer[0].nPosition;

	float nX1 = xyz[0].GetX();	float nY1 = xyz[0].GetY();	float nZ1 = xyz[0].GetZ();
	float nX2 = xyz[1].GetX();	float nY2 = xyz[1].GetY();	float nZ2 = xyz[1].GetZ();

	rgbaq[0] <<= m_VtxBuffer[1].nRGBAQ;
	rgbaq[1] <<= m_VtxBuffer[0].nRGBAQ;

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;

//	assert(nZ1 == nZ2);

	nZ1 = GetZ(nZ1);
	nZ2 = GetZ(nZ2);

	if(m_PrimitiveMode.nAlpha)
	{
		glEnable(GL_BLEND);
	}

	if(m_PrimitiveMode.nTexture)
	{
		float nS[2], nT[2];

		glColor4ub(MulBy2Clamp(rgbaq[0].nR), MulBy2Clamp(rgbaq[0].nG), MulBy2Clamp(rgbaq[0].nB), MulBy2Clamp(rgbaq[0].nA));

		if(m_PrimitiveMode.nUseUV)
		{
			UV uv[2];
			uv[0] <<= m_VtxBuffer[1].nUV;
			uv[1] <<= m_VtxBuffer[0].nUV;

			nS[0] = uv[0].GetU() / static_cast<float>(m_nTexWidth);
			nS[1] = uv[1].GetU() / static_cast<float>(m_nTexWidth);

			nT[0] = uv[0].GetV() / static_cast<float>(m_nTexHeight);
			nT[1] = uv[1].GetV() / static_cast<float>(m_nTexHeight);
		}
		else
		{
			ST st[2];

			st[0] <<= m_VtxBuffer[1].nST;
			st[1] <<= m_VtxBuffer[0].nST;

			float nQ1 = rgbaq[1].nQ;
			float nQ2 = rgbaq[0].nQ;
			if(nQ1 == 0) nQ1 = 1;
			if(nQ2 == 0) nQ2 = 1;

			nS[0] = st[0].nS / nQ1;
			nS[1] = st[1].nS / nQ2;

			nT[0] = st[0].nT / nQ1;
			nT[1] = st[1].nT / nQ2;
		}

		glBegin(GL_QUADS);
		{
			glTexCoord2f(nS[0], nT[0]);
			glVertex3f(nX1, nY1, nZ1);

			glTexCoord2f(nS[1], nT[0]);
			glVertex3f(nX2, nY1, nZ2);

			glTexCoord2f(nS[1], nT[1]);
			glVertex3f(nX2, nY2, nZ1);

			glTexCoord2f(nS[0], nT[1]);
			glVertex3f(nX1, nY2, nZ2);

		}
		glEnd();
	}
	else if(!m_PrimitiveMode.nTexture)
	{
		//REMOVE
		//Humm? Would it be possible to have a gradient using those registers?
		glColor4ub(MulBy2Clamp(rgbaq[0].nR), MulBy2Clamp(rgbaq[0].nG), MulBy2Clamp(rgbaq[0].nB), MulBy2Clamp(rgbaq[0].nA));
		//glColor4ub(rgbaq[0].nR, rgbaq[0].nG, rgbaq[0].nB, rgbaq[0].nA);

		glBegin(GL_QUADS);

			glVertex3f(nX1, nY1, nZ1);
			glVertex3f(nX2, nY1, nZ2);
			glVertex3f(nX2, nY2, nZ1);
			glVertex3f(nX1, nY2, nZ2);

		glEnd();
	}
	else
	{
		assert(0);
	}

	if(m_PrimitiveMode.nAlpha)
	{
		glDisable(GL_BLEND);
	}
}

/////////////////////////////////////////////////////////////
// Other Functions
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::WriteRegisterImpl(uint8 nRegister, uint64 nData)
{
	CGSHandler::WriteRegisterImpl(nRegister, nData);

	switch(nRegister)
	{
	case GS_REG_PRIM:
		m_nPrimitiveType = (unsigned int)(nData & 0x07);
		switch(m_nPrimitiveType)
		{
		case 0:
			//Point
			m_nVtxCount = 1;
			break;
		case 1:
			//Line
			m_nVtxCount = 2;
			break;
		case 2:
			//Line strip
			m_nVtxCount = 2;
			break;
		case 3:
			//Triangle
			m_nVtxCount = 3;
			break;
		case 4:
			//Triangle Strip
			m_nVtxCount = 3;
			break;
		case 5:
			//Triangle Fan
			m_nVtxCount = 3;
			break;
		case 6:
			//Sprite (rectangle)
			m_nVtxCount = 2;
			break;
		default:
			printf("GS: Unhandled primitive type (%i) encountered.\r\n", m_nPrimitiveType);
			break;
		}
		break;

	case GS_REG_XYZ2:
	case GS_REG_XYZ3:
	case GS_REG_XYZF2:
	case GS_REG_XYZF3:
		VertexKick(nRegister, nData);
		break;

	case GS_REG_FOGCOL:
		SetupFogColor();
		break;
	}
}

void CGSH_OpenGL::VertexKick(uint8 nRegister, uint64 nValue)
{
	if(m_nVtxCount == 0) return;

	bool nDrawingKick = (nRegister == GS_REG_XYZ2) || (nRegister == GS_REG_XYZF2);
	bool nFog = (nRegister == GS_REG_XYZF2) || (nRegister == GS_REG_XYZF3);

	if(nFog)
	{
		m_VtxBuffer[m_nVtxCount - 1].nPosition	= nValue & 0x00FFFFFFFFFFFFFFULL;
		m_VtxBuffer[m_nVtxCount - 1].nRGBAQ		= m_nReg[GS_REG_RGBAQ];
		m_VtxBuffer[m_nVtxCount - 1].nUV		= m_nReg[GS_REG_UV];
		m_VtxBuffer[m_nVtxCount - 1].nST		= m_nReg[GS_REG_ST];
		m_VtxBuffer[m_nVtxCount - 1].nFog		= (uint8)(nValue >> 56);
	}
	else
	{
		m_VtxBuffer[m_nVtxCount - 1].nPosition	= nValue;
		m_VtxBuffer[m_nVtxCount - 1].nRGBAQ		= m_nReg[GS_REG_RGBAQ];
		m_VtxBuffer[m_nVtxCount - 1].nUV		= m_nReg[GS_REG_UV];
		m_VtxBuffer[m_nVtxCount - 1].nST		= m_nReg[GS_REG_ST];
		m_VtxBuffer[m_nVtxCount - 1].nFog		= (uint8)(m_nReg[GS_REG_FOG] >> 56);
	}

	m_nVtxCount--;

	if(m_nVtxCount == 0)
	{
		if(nDrawingKick)
		{
			m_drawCallCount++;
		}

		{
			if((m_nReg[GS_REG_PRMODECONT] & 1) != 0)
			{
				m_PrimitiveMode <<= m_nReg[GS_REG_PRIM];
			}
			else
			{
				m_PrimitiveMode <<= m_nReg[GS_REG_PRMODE];
			}

			SetRenderingContext(m_PrimitiveMode.nContext);

			switch(m_nPrimitiveType)
			{
			case 0:
				if(nDrawingKick) Prim_Point();
				break;
			case 1:
				if(nDrawingKick) Prim_Line();
				break;
			case 2:
				if(nDrawingKick) Prim_Line();
				memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
				m_nVtxCount = 1;
				break;
			case 3:
				if(nDrawingKick) Prim_Triangle();
				m_nVtxCount = 3;
				break;
			case 4:
				if(nDrawingKick) Prim_Triangle();
				memcpy(&m_VtxBuffer[2], &m_VtxBuffer[1], sizeof(VERTEX));
				memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
				m_nVtxCount = 1;
				break;
			case 5:
				if(nDrawingKick) Prim_Triangle();
				memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
				m_nVtxCount = 1;
				break;
			case 6:
				if(nDrawingKick) Prim_Sprite();
				m_nVtxCount = 2;
				break;
			}
		}
	}
}

void CGSH_OpenGL::ProcessImageTransfer(uint32 nAddress, uint32 nLength, bool dirty)
{
	if(dirty)
	{
		TexCache_InvalidateTextures(nAddress, nLength);
	}

	//Invalidate any framebuffer that might have been overwritten by texture data that can't be displayed
	for(auto framebufferIterator(std::begin(m_framebuffers));
		framebufferIterator != std::end(m_framebuffers); framebufferIterator++)
	{
		const auto& framebuffer(*framebufferIterator);
		if(framebuffer->m_basePtr == nAddress)
		{
			framebuffer->m_canBeUsedAsTexture = false;
		}
	}

	BITBLTBUF bltBuf;
	bltBuf <<= m_nReg[GS_REG_BITBLTBUF];
	auto dstFramebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(), 
		[&] (const FramebufferPtr& framebuffer) 
		{
			return 
				framebuffer->m_basePtr == bltBuf.GetDstPtr() &&
				framebuffer->m_width == bltBuf.GetDstWidth() &&
				framebuffer->m_psm == bltBuf.nDstPsm;
		}
	);
	if(dstFramebufferIterator != std::end(m_framebuffers))
	{
		const auto& dstFramebuffer = (*dstFramebufferIterator);

		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();

		glViewport(0, 0, dstFramebuffer->m_width, dstFramebuffer->m_height);
		glScissor(0, 0, dstFramebuffer->m_width, dstFramebuffer->m_height);

		float projWidth = static_cast<float>(dstFramebuffer->m_width);
		float projHeight = static_cast<float>(dstFramebuffer->m_height);
		LinearZOrtho(0, projWidth, 0, projHeight);

		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();

		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		glBindFramebuffer(GL_FRAMEBUFFER, dstFramebuffer->m_framebuffer);

		DisplayTransferedImage(nAddress);

		m_renderState.isValid = false;
	}
}

void CGSH_OpenGL::ProcessClutTransfer(uint32 csa, uint32)
{
	PalCache_Invalidate(csa);
}

void CGSH_OpenGL::ProcessLocalToLocalTransfer()
{
	BITBLTBUF bltBuf;
	bltBuf <<= m_nReg[GS_REG_BITBLTBUF];
	auto srcFramebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(), 
		[&] (const FramebufferPtr& framebuffer) 
		{
			return framebuffer->m_basePtr == bltBuf.GetSrcPtr();
		}
	);
	auto dstFramebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(), 
		[&] (const FramebufferPtr& framebuffer) 
		{
			return framebuffer->m_basePtr == bltBuf.GetDstPtr();
		}
	);
	if(
		srcFramebufferIterator != std::end(m_framebuffers) && 
		dstFramebufferIterator != std::end(m_framebuffers))
	{
		const auto& srcFramebuffer = (*srcFramebufferIterator);
		const auto& dstFramebuffer = (*dstFramebufferIterator);

		glBindFramebuffer(GL_FRAMEBUFFER, dstFramebuffer->m_framebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFramebuffer->m_framebuffer);

		//Copy buffers
		glBlitFramebuffer(
			0, 0, srcFramebuffer->m_width, srcFramebuffer->m_height, 
			0, 0, srcFramebuffer->m_width, srcFramebuffer->m_height, 
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
		assert(glGetError() == GL_NO_ERROR);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

		m_renderState.isValid = false;
	}
}

void CGSH_OpenGL::ReadFramebuffer(uint32 width, uint32 height, void* buffer)
{
	glFlush();
	glFinish();
	glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, buffer);
}

void CGSH_OpenGL::DisplayTransferedImage(uint32 nAddress)
{
	TRXREG trxReg;
	TRXPOS trxPos;

	trxReg <<= m_nReg[GS_REG_TRXREG];
	trxPos <<= m_nReg[GS_REG_TRXPOS];

	unsigned int nW = trxReg.nRRW;
	unsigned int nH = trxReg.nRRH;

	unsigned int nDX = trxPos.nDSAX;
	unsigned int nDY = trxPos.nDSAY;

	glUseProgram(0);

	GLuint nTexture = 0;
	glGenTextures(1, &nTexture);

	glBindTexture(GL_TEXTURE_2D, nTexture);

	unsigned int nW2 = GetNextPowerOf2(nW);
	unsigned int nH2 = GetNextPowerOf2(nH);

	FetchImagePSMCT32(reinterpret_cast<uint32*>(m_pCvtBuffer), nAddress, nW / 64, nW, nH);

	//Upload the texture
	glPixelStorei(GL_UNPACK_ROW_LENGTH, nW);

	glTexImage2D(GL_TEXTURE_2D, 0, 4, nW2, nH2, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_pCvtBuffer);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

	glColor4f(1.0, 1.0, 1.0, 1.0);

	float nS = (float)nW / (float)nW2;
	float nT = (float)nH / (float)nH2;

	glBegin(GL_QUADS);
		
		glTexCoord2f(0.0f,		0.0f);
		glVertex2f(nDX,			nDY);

		glTexCoord2f(0.0f,		nT);
		glVertex2f(nDX,			nDY + nH);

		glTexCoord2f(nS,		nT);
		glVertex2f(nDX + nW,	nDY + nH);

		glTexCoord2f(nS,		0.0f);
		glVertex2f(nDX + nW,	nDY);

	glEnd();

	glBindTexture(GL_TEXTURE_2D, NULL);

	glDeleteTextures(1, &nTexture);
}

bool CGSH_OpenGL::IsBlendColorExtSupported()
{
	return glBlendColorEXT != NULL;
}

bool CGSH_OpenGL::IsBlendEquationExtSupported()
{
	return glBlendEquationEXT != NULL;
}

bool CGSH_OpenGL::IsRGBA5551ExtSupported()
{
	return m_nIsRGBA5551Supported;
}

bool CGSH_OpenGL::IsFogCoordfExtSupported()
{
	return glFogCoordfEXT != NULL;
}

/////////////////////////////////////////////////////////////
// Framebuffer
/////////////////////////////////////////////////////////////

CGSH_OpenGL::CFramebuffer::CFramebuffer(uint32 basePtr, uint32 width, uint32 height, uint32 psm)
: m_basePtr(basePtr)
, m_width(width)
, m_height(height)
, m_psm(psm)
, m_framebuffer(0)
, m_texture(0)
, m_depthBuffer(0)
, m_canBeUsedAsTexture(false)
{
	//Build color attachment
	glGenTextures(1, &m_texture);
	glBindTexture(GL_TEXTURE_2D, m_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	assert(glGetError() == GL_NO_ERROR);
		
	//Build depth attachment
	glGenRenderbuffers(1, &m_depthBuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, m_depthBuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, m_width, m_height);
	assert(glGetError() == GL_NO_ERROR);

	//Build framebuffer
	glGenFramebuffers(1, &m_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
	glFramebufferTextureEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_texture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthBuffer);
	assert(glGetError() == GL_NO_ERROR);

	GLenum result = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	assert(result == GL_FRAMEBUFFER_COMPLETE);

	GLenum drawBufferId = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &drawBufferId);

	assert(glGetError() == GL_NO_ERROR);
}

CGSH_OpenGL::CFramebuffer::~CFramebuffer()
{
	if(m_framebuffer != 0)
	{
		glDeleteFramebuffers(1, &m_framebuffer);
	}
	if(m_texture != 0)
	{
		glDeleteTextures(1, &m_texture);
	}
	if(m_depthBuffer != 0)
	{
		glDeleteRenderbuffers(1, &m_depthBuffer);
	}
}

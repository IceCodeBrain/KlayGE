/**
 * @file ToolCommonLoader.hpp
 * @author Minmin Gong
 *
 * @section DESCRIPTION
 *
 * This source file is part of KlayGE
 * For the latest info, see http://www.klayge.org
 *
 * @section LICENSE
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * You may alternatively use this source under the terms of
 * the KlayGE Proprietary License (KPL). You can obtained such a license
 * from http://www.klayge.org/licensing/.
 */

#ifndef KLAYGE_CORE_BASE_TOOL_COMMON_LOADER_HPP
#define KLAYGE_CORE_BASE_TOOL_COMMON_LOADER_HPP

#pragma once

#if KLAYGE_IS_DEV_PLATFORM

#include <KlayGE/PreDeclare.hpp>

#include <KFL/CXX17/string_view.hpp>
#include <KFL/DllLoader.hpp>

namespace KlayGE
{
	class ToolCommonLoader
	{
	public:
		static ToolCommonLoader& Instance();

		void ConvertModel(std::string_view input_name, std::string_view metadata_name, std::string_view output_name,
			RenderDeviceCaps const * caps);
		void ConvertTexture(std::string_view input_name, std::string_view metadata_name, std::string_view output_name,
			RenderDeviceCaps const * caps);

	private:
		ToolCommonLoader();

	private:
		using ConvertModelFunc = void(*)(std::string_view tex_name, std::string_view metadata_name, std::string_view output_name,
			RenderDeviceCaps const * caps);
		using ConvertTextureFunc = void(*)(std::string_view tex_name, std::string_view metadata_name, std::string_view output_name,
			RenderDeviceCaps const * caps);

		ConvertModelFunc DynamicConvertModel_;
		ConvertTextureFunc DynamicConvertTexture_;

		DllLoader dll_loader_;
	};
}

#endif

#endif			// KLAYGE_CORE_BASE_TOOL_COMMON_LOADER_HPP

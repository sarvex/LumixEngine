#include <imgui/imgui.h>

#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/property_grid.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/allocator.h"
#include "engine/array.h"
#include "engine/crt.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/hash.h"
#include "engine/log.h"
#include "engine/os.h"
#include "engine/path.h"
#include "engine/stream.h"
#include "engine/world.h"
#include "lua_script/lua_script.h"
#include "lua_script/lua_script_system.h"
#include <lua.hpp>


using namespace Lumix;


static const ComponentType LUA_SCRIPT_TYPE = reflection::getComponentType("lua_script");


namespace
{


struct AssetPlugin : AssetBrowser::Plugin, AssetCompiler::IPlugin
{
	explicit AssetPlugin(StudioApp& app)
		: m_app(app)
		, AssetBrowser::Plugin(app.getAllocator())
	{
		app.getAssetCompiler().registerExtension("lua", LuaScript::TYPE);
		m_text_buffer[0] = 0;
	}

	void deserialize(InputMemoryStream& blob) override { ASSERT(false); }
	void serialize(OutputMemoryStream& blob) override {}

	bool compile(const Path& src) override
	{
		return m_app.getAssetCompiler().copyCompile(src);
	}

	
	bool onGUI(Span<Resource*> resources) override
	{
		if (resources.length() > 1) return false;

		auto* script = static_cast<LuaScript*>(resources[0]);

		if (m_text_buffer[0] == '\0')
		{
			m_too_long = !copyString(m_text_buffer, script->getSourceCode());
		}
		ImGui::SetNextItemWidth(-1);
		if (!m_too_long) {
			ImGui::InputTextMultiline("##code", m_text_buffer, sizeof(m_text_buffer), ImVec2(0, 300));
			if (ImGui::Button(ICON_FA_SAVE "Save"))
			{
				FileSystem& fs = m_app.getEngine().getFileSystem();
				if (!fs.saveContentSync(script->getPath(), Span((const u8*)m_text_buffer, stringLength(m_text_buffer)))) {
					logWarning("Could not save ", script->getPath());
					return false;
				}
			}
			ImGui::SameLine();
		}
		else {
			ImGui::Text(ICON_FA_EXCLAMATION_TRIANGLE "File is too big to be edited here, please use external editor");
		}
		if (ImGui::Button(ICON_FA_EXTERNAL_LINK_ALT "Open externally"))
		{
			m_app.getAssetBrowser().openInExternalEditor(script);
		}
		return false;
	}


	void onResourceUnloaded(Resource*) override { m_text_buffer[0] = 0; }
	const char* getName() const override { return "Lua Script"; }


	ResourceType getResourceType() const override { return LuaScript::TYPE; }


	bool createTile(const char* in_path, const char* out_path, ResourceType type) override
	{
		if (type == LuaScript::TYPE)
		{
			return m_app.getAssetBrowser().copyTile("editor/textures/tile_lua_script.tga", out_path);
		}
		return false;
	}


	StudioApp& m_app;
	char m_text_buffer[8192];
	bool m_too_long = false;
};


struct ConsolePlugin final : StudioApp::GUIPlugin
{
	explicit ConsolePlugin(StudioApp& _app)
		: app(_app)
		, open(false)
		, autocomplete(_app.getAllocator())
	{
		m_toggle_ui.init("Script Console", "Toggle script console", "script_console", "", true);
		m_toggle_ui.func.bind<&ConsolePlugin::toggleOpen>(this);
		m_toggle_ui.is_selected.bind<&ConsolePlugin::isOpen>(this);
		app.addWindowAction(&m_toggle_ui);
		buf[0] = '\0';
	}

	~ConsolePlugin() {
		app.removeAction(&m_toggle_ui);
	}

	void onSettingsLoaded() override {
		Settings& settings = app.getSettings();
		open = settings.getValue(Settings::GLOBAL, "is_script_console_open", false);
		if (!buf[0]) {
			Span<const char> dir = Path::getDir(settings.getAppDataPath());
			const StaticString<LUMIX_MAX_PATH> path(dir, "/lua_console_content.lua");
			os::InputFile file;
			if (file.open(path)) {
				const u64 size = file.size();
				if (size + 1 <= sizeof(buf)) {
					if (!file.read(buf, size)) {
						logError("Failed to read ", path);
						buf[0] = '\0';
					}
					else {
						buf[size] = '\0';
					}
				}
				file.close();
			}
		}
	}

	void onBeforeSettingsSaved() override {
		Settings& settings = app.getSettings();
		settings.setValue(Settings::GLOBAL, "is_script_console_open", open);
		if (buf[0]) {
			Span<const char> dir = Path::getDir(settings.getAppDataPath());
			const StaticString<LUMIX_MAX_PATH> path(dir, "/lua_console_content.lua");
			os::OutputFile file;
			if (!file.open(path)) {
				logError("Failed to save ", path);
			}
			else {
				if (!file.write(buf, stringLength(buf))) {
					logError("Failed to write ", path);
				}
				file.close();
			}
		}
	}

	/*static const int LUA_CALL_EVENT_SIZE = 32;

	void pluginAdded(GUIPlugin& plugin) override
	{
		if (!equalStrings(plugin.getName(), "animation_editor")) return;

		auto& anim_editor = (AnimEditor::IAnimationEditor&)plugin;
		auto& event_type = anim_editor.createEventType("lua_call");
		event_type.size = LUA_CALL_EVENT_SIZE;
		event_type.label = "Lua call";
		event_type.editor.bind<ConsolePlugin, &ConsolePlugin::onLuaCallEventGUI>(this);
	}


	void onLuaCallEventGUI(u8* data, AnimEditor::Component& component) const
	{
		LuaScriptScene* scene = (LuaScriptScene*)app.getWorldEditor().getWorld()->getScene(LUA_SCRIPT_TYPE);
		ImGui::InputText("Function", (char*)data, LUA_CALL_EVENT_SIZE);
	}
	*/

	const char* getName() const override { return "script_console"; }


	bool isOpen() const { return open; }
	void toggleOpen() { open = !open; }


	void autocompleteSubstep(lua_State* L, const char* str, ImGuiInputTextCallbackData *data)
	{
		char item[128];
		const char* next = str;
		char* c = item;
		while (*next != '.' && *next != '\0')
		{
			*c = *next;
			++next;
			++c;
		}
		*c = '\0';

		if (!lua_istable(L, -1)) return;

		lua_pushnil(L);
		while (lua_next(L, -2) != 0)
		{
			const char* name = lua_tostring(L, -2);
			if (startsWith(name, item))
			{
				if (*next == '.' && next[1] == '\0')
				{
					autocompleteSubstep(L, "", data);
				}
				else if (*next == '\0')
				{
					autocomplete.push(String(name, app.getAllocator()));
				}
				else
				{
					autocompleteSubstep(L, next + 1, data);
				}
			}
			lua_pop(L, 1);
		}
	}


	static bool isWordChar(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
	}


	static int autocompleteCallback(ImGuiInputTextCallbackData *data)
	{
		auto* that = (ConsolePlugin*)data->UserData;
		if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
		{
			lua_State* L = that->app.getEngine().getState();

			int start_word = data->CursorPos;
			char c = data->Buf[start_word - 1];
			while (start_word > 0 && (isWordChar(c) || c == '.'))
			{
				--start_word;
				c = data->Buf[start_word - 1];
			}
			char tmp[128];
			copyNString(Span(tmp), data->Buf + start_word, data->CursorPos - start_word);

			that->autocomplete.clear();
			lua_pushvalue(L, LUA_GLOBALSINDEX);
			that->autocompleteSubstep(L, tmp, data);
			lua_pop(L, 1);
			if (!that->autocomplete.empty())
			{
				that->open_autocomplete = true;
				qsort(&that->autocomplete[0],
					that->autocomplete.size(),
					sizeof(that->autocomplete[0]),
					[](const void* a, const void* b) {
					const char* a_str = ((const String*)a)->c_str();
					const char* b_str = ((const String*)b)->c_str();
					return compareString(a_str, b_str);
				});
			}
		}
		else if (that->insert_value)
		{
			int start_word = data->CursorPos;
			char c = data->Buf[start_word - 1];
			while (start_word > 0 && (isWordChar(c)))
			{
				--start_word;
				c = data->Buf[start_word - 1];
			}
			data->InsertChars(data->CursorPos, that->insert_value + data->CursorPos - start_word);
			that->insert_value = nullptr;
		}
		return 0;
	}


	void onWindowGUI() override
	{
		if (!open) return;
		if (ImGui::Begin(ICON_FA_SCROLL "Lua console##lua_console", &open))
		{
			if (ImGui::Button("Execute"))
			{
				lua_State* L = app.getEngine().getState();
				
				bool errors = luaL_loadbuffer(L, buf, stringLength(buf), nullptr) != 0;
				errors = errors || lua_pcall(L, 0, 0, 0) != 0;

				if (errors)
				{
					logError(lua_tostring(L, -1));
					lua_pop(L, 1);
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Execute file"))
			{
				char tmp[LUMIX_MAX_PATH] = {};
				if (os::getOpenFilename(Span(tmp), "Scripts\0*.lua\0", nullptr))
				{
					os::InputFile file;
					IAllocator& allocator = app.getAllocator();
					if (file.open(tmp))
					{
						size_t size = file.size();
						Array<char> data(allocator);
						data.resize((int)size);
						if (!file.read(&data[0], size)) {
							logError("Could not read ", tmp);
							data.clear();
						}
						file.close();
						lua_State* L = app.getEngine().getState();
						bool errors = luaL_loadbuffer(L, &data[0], data.size(), tmp) != 0;
						errors = errors || lua_pcall(L, 0, 0, 0) != 0;

						if (errors)
						{
							logError(lua_tostring(L, -1));
							lua_pop(L, 1);
						}
					}
					else
					{
						logError("Failed to open file ", tmp);
					}
				}
			}
			if(insert_value) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextMultiline("##repl",
				buf,
				lengthOf(buf),
				ImVec2(-1, -1),
				ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackCompletion,
				autocompleteCallback,
				this);

			if (open_autocomplete)
			{
				ImGui::OpenPopup("autocomplete");
				ImGui::SetNextWindowPos(ImGuiEx::GetOsImePosRequest());
			}
			open_autocomplete = false;
			if (ImGui::BeginPopup("autocomplete"))
			{
				if (autocomplete.size() == 1)
				{
					insert_value = autocomplete[0].c_str();
				}
				if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++autocomplete_selected;
				if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) --autocomplete_selected;
				if (ImGui::IsKeyPressed(ImGuiKey_Enter)) insert_value = autocomplete[autocomplete_selected].c_str();
				if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
				autocomplete_selected = clamp(autocomplete_selected, 0, autocomplete.size() - 1);
				for (int i = 0, c = autocomplete.size(); i < c; ++i)
				{
					const char* value = autocomplete[i].c_str();
					if (ImGui::Selectable(value, autocomplete_selected == i))
					{
						insert_value = value;
					}
				}
				ImGui::EndPopup();
			}
		}
		ImGui::End();
	}


	StudioApp& app;
	Action m_toggle_ui;
	Array<String> autocomplete;
	bool open;
	bool open_autocomplete = false;
	int autocomplete_selected = 1;
	const char* insert_value = nullptr;
	char buf[10 * 1024];
};


struct AddComponentPlugin final : StudioApp::IAddComponentPlugin
{
	explicit AddComponentPlugin(StudioApp& app)
		: app(app)
		, file_selector("lua", app)
	{
	}


	void onGUI(bool create_entity, bool, EntityPtr parent, WorldEditor& editor) override
	{
		if (!ImGui::BeginMenu("File")) return;
		char buf[LUMIX_MAX_PATH];
		AssetBrowser& asset_browser = app.getAssetBrowser();
		bool new_created = false;
		if (ImGui::BeginMenu("New")) {
			file_selector.gui(false);
			if (ImGui::Button("Create")) {
				copyString(Span(buf), file_selector.getPath());
				os::OutputFile file;
				FileSystem& fs = app.getEngine().getFileSystem();
				if (fs.open(file_selector.getPath(), file)) {
					new_created = true;
					file.close();
				}
				else {
					logError("Failed to create ", buf);
				}
			}
			ImGui::EndMenu();
		}
		bool create_empty = ImGui::Selectable("Empty", false);

		static FilePathHash selected_res_hash;
		if (asset_browser.resourceList(Span(buf), selected_res_hash, LuaScript::TYPE, false) || create_empty || new_created)
		{
			editor.beginCommandGroup("createEntityWithComponent");
			if (create_entity)
			{
				EntityRef entity = editor.addEntity();
				editor.selectEntities(Span(&entity, 1), false);
			}
			if (editor.getSelectedEntities().empty()) return;
			EntityRef entity = editor.getSelectedEntities()[0];

			if (!editor.getWorld()->hasComponent(entity, LUA_SCRIPT_TYPE))
			{
				editor.addComponent(Span(&entity, 1), LUA_SCRIPT_TYPE);
			}

			const ComponentUID cmp = editor.getWorld()->getComponent(entity, LUA_SCRIPT_TYPE);
			editor.beginCommandGroup("add_lua_script");
			editor.addArrayPropertyItem(cmp, "scripts");

			if (!create_empty) {
				auto* script_scene = static_cast<LuaScriptScene*>(editor.getWorld()->getScene(LUA_SCRIPT_TYPE));
				int scr_count = script_scene->getScriptCount(entity);
				editor.setProperty(cmp.type, "scripts", scr_count - 1, "Path", Span((const EntityRef*)&entity, 1), Path(buf));
			}
			editor.endCommandGroup();
			if (parent.isValid()) editor.makeParent(parent, entity);
			editor.endCommandGroup();
			editor.lockGroupCommand();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndMenu();
	}


	const char* getLabel() const override 
	{
		return "Lua Script / File";
	}


	StudioApp& app;
	FileSelector file_selector;
};

struct PropertyGridPlugin final : PropertyGrid::IPlugin
{
	void onGUI(PropertyGrid& grid, Span<const EntityRef> entities, ComponentType cmp_type, WorldEditor& editor) override {
		if (cmp_type != LUA_SCRIPT_TYPE) return;
		if (entities.length() != 1) return;

		LuaScriptScene* scene = (LuaScriptScene*)editor.getWorld()->getScene(cmp_type); 
		const EntityRef e = entities[0];
		const u32 count = scene->getScriptCount(e);
		for (u32 i = 0; i < count; ++i) {
			if (scene->beginFunctionCall(e, i, "onGUI")) {
				scene->endFunctionCall();
			}
		}
	}
};

struct StudioAppPlugin : StudioApp::IPlugin
{
	StudioAppPlugin(StudioApp& app)
		: m_app(app)
		, m_asset_plugin(app)
		, m_console_plugin(app)
	{
	}

	const char* getName() const override { return "lua_script"; }

	void init() override
	{
		AddComponentPlugin* add_cmp_plugin = LUMIX_NEW(m_app.getAllocator(), AddComponentPlugin)(m_app);
		m_app.registerComponent(ICON_FA_MOON, "lua_script", *add_cmp_plugin);

		const char* exts[] = { "lua", nullptr };
		m_app.getAssetCompiler().addPlugin(m_asset_plugin, exts);
		m_app.getAssetBrowser().addPlugin(m_asset_plugin);
		m_app.addPlugin(m_console_plugin);
		m_app.getPropertyGrid().addPlugin(m_property_grid_plugin);
	}

	~StudioAppPlugin()
	{
		m_app.getAssetCompiler().removePlugin(m_asset_plugin);
		m_app.getAssetBrowser().removePlugin(m_asset_plugin);
		m_app.removePlugin(m_console_plugin);
		m_app.getPropertyGrid().removePlugin(m_property_grid_plugin);
	}

	bool showGizmo(WorldView& view, ComponentUID cmp) override
	{
		if (cmp.type == LUA_SCRIPT_TYPE)
		{
			auto* scene = static_cast<LuaScriptScene*>(cmp.scene);
			int count = scene->getScriptCount((EntityRef)cmp.entity);
			for (int i = 0; i < count; ++i)
			{
				if (scene->beginFunctionCall((EntityRef)cmp.entity, i, "onDrawGizmo"))
				{
					scene->endFunctionCall();
				}
			}
			return true;
		}
		return false;
	}
	
	StudioApp& m_app;
	AssetPlugin m_asset_plugin;
	ConsolePlugin m_console_plugin;
	PropertyGridPlugin m_property_grid_plugin;
};


} // anonymous namespace


LUMIX_STUDIO_ENTRY(lua_script)
{
	IAllocator& allocator = app.getAllocator();
	return LUMIX_NEW(allocator, StudioAppPlugin)(app);
}



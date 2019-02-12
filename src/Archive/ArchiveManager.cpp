
// -----------------------------------------------------------------------------
// SLADE - It's a Doom Editor
// Copyright(C) 2008 - 2019 Simon Judd
//
// Email:       sirjuddington@gmail.com
// Web:         http://slade.mancubus.net
// Filename:    ArchiveManager.cpp
// Description: ArchiveManager class. Manages all open archives and the
//              interactions between them
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110 - 1301, USA.
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
//
// Includes
//
// -----------------------------------------------------------------------------
#include "Main.h"
#include "ArchiveManager.h"
#include "App.h"
#include "Formats/All.h"
#include "Formats/DirArchive.h"
#include "General/Console/Console.h"
#include "General/ResourceManager.h"
#include "General/UI.h"
#include "Utility/StringUtils.h"


// -----------------------------------------------------------------------------
//
// Variables
//
// -----------------------------------------------------------------------------
CVAR(Int, base_resource, -1, CVar::Flag::Save)
CVAR(Int, max_recent_files, 25, CVar::Flag::Save)
CVAR(Bool, auto_open_wads_root, false, CVar::Flag::Save)


// -----------------------------------------------------------------------------
//
// ArchiveManager Class Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// ArchiveManager class destructor
// -----------------------------------------------------------------------------
ArchiveManager::~ArchiveManager()
{
	clearAnnouncers();
}

// -----------------------------------------------------------------------------
// Checks that the given directory is actually a suitable resource directory
// for SLADE 3, and not just a directory named 'res' that happens to be there
// (possibly because the user installed SLADE in the same folder as an
// installation of SLumpEd).
// -----------------------------------------------------------------------------
bool ArchiveManager::validResDir(const wxString& dir) const
{
	// Assortment of resources that the program expects to find.
	// If at least one is missing, then probably more are missing
	// too, so the res folder cannot be used.
	wxString paths[] = {
		"animated.lmp",
		"config/executables.cfg",
		"config/nodebuilders.cfg",
		"fonts/dejavu_sans.ttf",
		"html/box-title-back.png",
		"html/startpage.htm",
		"icons/entry_list/archive.png",
		"icons/general/wiki.png",
		"images/arrow.png",
		"logo.png",
		"palettes/Doom .pal",
		"s3dummy.lmp",
		"slade.ico",
		"switches.lmp",
		"tips.txt",
		"vga-rom-font.16",
	};

	for (const auto& path : paths)
	{
		wxFileName fn(dir + "/" + path);
		if (!wxFileExists(fn.GetFullPath()))
		{
			Log::info(wxString::Format(
				"Resource %s was not found in dir %s!\n"
				"This resource folder cannot be used. "
				"(Did you install SLADE 3 in a SLumpEd folder?)",
				path,
				dir));
			return false;
		}
	}
	return true;
}

// -----------------------------------------------------------------------------
// Initialised the ArchiveManager. Finds and opens the program resource archive
// -----------------------------------------------------------------------------
bool ArchiveManager::init()
{
	program_resource_archive_ = std::make_unique<ZipArchive>();

#ifdef __WXOSX__
	string resdir = App::path("../Resources", App::Dir::Executable); // Use Resources dir within bundle on mac
#else
	wxString resdir = App::path("res", App::Dir::Executable);
#endif

	if (wxDirExists(resdir) && validResDir(resdir))
	{
		program_resource_archive_->importDir(resdir);
		res_archive_open_ = (program_resource_archive_->numEntries() > 0);

		if (!initArchiveFormats())
			Log::error("An error occurred reading archive formats configuration");

		return res_archive_open_;
	}

	// Find slade3.pk3 directory
	wxString dir_slade_pk3 = App::path("slade.pk3", App::Dir::Resources);
	if (!wxFileExists(dir_slade_pk3))
		dir_slade_pk3 = App::path("slade.pk3", App::Dir::Data);
	if (!wxFileExists(dir_slade_pk3))
		dir_slade_pk3 = App::path("slade.pk3", App::Dir::Executable);
	if (!wxFileExists(dir_slade_pk3))
		dir_slade_pk3 = App::path("slade.pk3", App::Dir::User);
	if (!wxFileExists(dir_slade_pk3))
		dir_slade_pk3 = "slade.pk3";

	// Open slade.pk3
	if (!program_resource_archive_->open(dir_slade_pk3))
	{
		Log::error("Unable to find slade.pk3!");
		res_archive_open_ = false;
	}
	else
		res_archive_open_ = true;

	if (!initArchiveFormats())
		Log::error("An error occurred reading archive formats configuration");

	return res_archive_open_;
}

// -----------------------------------------------------------------------------
// Loads archive formats configuration from the program resource
// -----------------------------------------------------------------------------
bool ArchiveManager::initArchiveFormats() const
{
	return Archive::loadFormats(program_resource_archive_->entryAtPath("config/archive_formats.cfg")->data());
}

// -----------------------------------------------------------------------------
// Initialises the base resource archive
// -----------------------------------------------------------------------------
bool ArchiveManager::initBaseResource()
{
	return openBaseResource((int)base_resource);
}

// -----------------------------------------------------------------------------
// Adds an archive to the archive list
// -----------------------------------------------------------------------------
bool ArchiveManager::addArchive(Archive* archive)
{
	// Only add if archive is a valid pointer
	if (archive)
	{
		// Add to the list
		OpenArchive n_archive;
		n_archive.archive  = archive;
		n_archive.resource = true;
		open_archives_.push_back(n_archive);

		// Listen to the archive
		listenTo(archive);

		// Announce the addition
		announce("archive_added");

		// Add to resource manager
		App::resources().addArchive(archive);

		// ZDoom also loads any WADs found in the root of a PK3 or directory
		if ((archive->formatId() == "zip" || archive->formatId() == "folder") && auto_open_wads_root)
		{
			for (const auto& entry : archive->rootDir()->allEntries())
			{
				if (entry->type() == EntryType::unknownType())
					EntryType::detectEntryType(entry.get());

				if (entry->type()->id() == "wad")
					// First true: yes, manage this
					// Second true: open silently, don't open a tab for it
					openArchive(entry.get(), true, true);
			}
		}

		return true;
	}
	else
		return false;
}

// -----------------------------------------------------------------------------
// Returns the archive at the index specified (nullptr if it doesn't exist)
// -----------------------------------------------------------------------------
Archive* ArchiveManager::getArchive(int index)
{
	// Check that index is valid
	if (index < 0 || index >= (int)open_archives_.size())
		return nullptr;
	else
		return open_archives_[index].archive;
}

// -----------------------------------------------------------------------------
// Returns the archive with the specified filename
// (nullptr if it doesn't exist)
// -----------------------------------------------------------------------------
Archive* ArchiveManager::getArchive(const wxString& filename)
{
	// Go through all open archives
	for (auto& open_archive : open_archives_)
	{
		// If the filename matches, return it
		if (open_archive.archive->filename().compare(filename) == 0)
			return open_archive.archive;
	}

	// If no archive is found with a matching filename, return nullptr
	return nullptr;
}

// -----------------------------------------------------------------------------
// Opens and adds a archive to the list, returns a pointer to the newly opened
// and added archive, or nullptr if an error occurred
// -----------------------------------------------------------------------------
Archive* ArchiveManager::openArchive(const wxString& filename, bool manage, bool silent)
{
	// Check for directory
	if (!wxFile::Exists(filename) && wxDirExists(filename))
		return openDirArchive(filename, manage, silent);

	auto new_archive = getArchive(filename);

	Log::info(wxString::Format("Opening archive %s", filename));

	// If the archive is already open, just return it
	if (new_archive)
	{
		// Announce open
		if (!silent)
		{
			MemChunk mc;
			uint32_t index = archiveIndex(new_archive);
			mc.write(&index, 4);
			announce("archive_opened", mc);
		}

		return new_archive;
	}

	// Determine file format
	if (WadArchive::isWadArchive(filename))
		new_archive = new WadArchive();
	else if (ZipArchive::isZipArchive(filename))
		new_archive = new ZipArchive();
	else if (ResArchive::isResArchive(filename))
		new_archive = new ResArchive();
	else if (DatArchive::isDatArchive(filename))
		new_archive = new DatArchive();
	else if (LibArchive::isLibArchive(filename))
		new_archive = new LibArchive();
	else if (PakArchive::isPakArchive(filename))
		new_archive = new PakArchive();
	else if (BSPArchive::isBSPArchive(filename))
		new_archive = new BSPArchive();
	else if (GrpArchive::isGrpArchive(filename))
		new_archive = new GrpArchive();
	else if (RffArchive::isRffArchive(filename))
		new_archive = new RffArchive();
	else if (GobArchive::isGobArchive(filename))
		new_archive = new GobArchive();
	else if (LfdArchive::isLfdArchive(filename))
		new_archive = new LfdArchive();
	else if (HogArchive::isHogArchive(filename))
		new_archive = new HogArchive();
	else if (ADatArchive::isADatArchive(filename))
		new_archive = new ADatArchive();
	else if (Wad2Archive::isWad2Archive(filename))
		new_archive = new Wad2Archive();
	else if (WadJArchive::isWadJArchive(filename))
		new_archive = new WadJArchive();
	else if (WolfArchive::isWolfArchive(filename))
		new_archive = new WolfArchive();
	else if (GZipArchive::isGZipArchive(filename))
		new_archive = new GZipArchive();
	else if (BZip2Archive::isBZip2Archive(filename))
		new_archive = new BZip2Archive();
	else if (TarArchive::isTarArchive(filename))
		new_archive = new TarArchive();
	else if (DiskArchive::isDiskArchive(filename))
		new_archive = new DiskArchive();
	else if (PodArchive::isPodArchive(filename))
		new_archive = new PodArchive();
	else if (ChasmBinArchive::isChasmBinArchive(filename))
		new_archive = new ChasmBinArchive();
	else if (SiNArchive::isSiNArchive(filename))
		new_archive = new SiNArchive();
	else
	{
		// Unsupported format
		Global::error = "Unsupported or invalid Archive format";
		return nullptr;
	}

	// If it opened successfully, add it to the list if needed & return it,
	// Otherwise, delete it and return nullptr
	if (new_archive->open(filename))
	{
		if (manage)
		{
			// Add the archive
			addArchive(new_archive);

			// Announce open
			if (!silent)
			{
				MemChunk mc;
				uint32_t index = archiveIndex(new_archive);
				mc.write(&index, 4);
				announce("archive_opened", mc);
			}

			// Add to recent files
			addRecentFile(filename);
		}

		// Return the opened archive
		return new_archive;
	}
	else
	{
		Log::error(Global::error);
		delete new_archive;
		return nullptr;
	}
}

// -----------------------------------------------------------------------------
// Same as the above function, except it opens from an ArchiveEntry
// -----------------------------------------------------------------------------
Archive* ArchiveManager::openArchive(ArchiveEntry* entry, bool manage, bool silent)
{
	// Check entry was given
	if (!entry)
		return nullptr;

	// Check if the entry is already opened
	for (auto& open_archive : open_archives_)
	{
		if (open_archive.archive->parentEntry() == entry)
		{
			// Announce open
			if (!silent)
			{
				MemChunk mc;
				uint32_t index = archiveIndex(open_archive.archive);
				mc.write(&index, 4);
				announce("archive_opened", mc);
			}

			return open_archive.archive;
		}
	}

	// Check entry type
	Archive* new_archive;
	if (WadArchive::isWadArchive(entry->data()))
		new_archive = new WadArchive();
	else if (ZipArchive::isZipArchive(entry->data()))
		new_archive = new ZipArchive();
	else if (ResArchive::isResArchive(entry->data()))
		new_archive = new ResArchive();
	else if (LibArchive::isLibArchive(entry->data()))
		new_archive = new LibArchive();
	else if (DatArchive::isDatArchive(entry->data()))
		new_archive = new DatArchive();
	else if (PakArchive::isPakArchive(entry->data()))
		new_archive = new PakArchive();
	else if (BSPArchive::isBSPArchive(entry->data()))
		new_archive = new BSPArchive();
	else if (GrpArchive::isGrpArchive(entry->data()))
		new_archive = new GrpArchive();
	else if (RffArchive::isRffArchive(entry->data()))
		new_archive = new RffArchive();
	else if (GobArchive::isGobArchive(entry->data()))
		new_archive = new GobArchive();
	else if (LfdArchive::isLfdArchive(entry->data()))
		new_archive = new LfdArchive();
	else if (HogArchive::isHogArchive(entry->data()))
		new_archive = new HogArchive();
	else if (ADatArchive::isADatArchive(entry->data()))
		new_archive = new ADatArchive();
	else if (Wad2Archive::isWad2Archive(entry->data()))
		new_archive = new Wad2Archive();
	else if (WadJArchive::isWadJArchive(entry->data()))
		new_archive = new WadJArchive();
	else if (WolfArchive::isWolfArchive(entry->data()))
		new_archive = new WolfArchive();
	else if (GZipArchive::isGZipArchive(entry->data()))
		new_archive = new GZipArchive();
	else if (BZip2Archive::isBZip2Archive(entry->data()))
		new_archive = new BZip2Archive();
	else if (TarArchive::isTarArchive(entry->data()))
		new_archive = new TarArchive();
	else if (DiskArchive::isDiskArchive(entry->data()))
		new_archive = new DiskArchive();
	else if (StrUtil::endsWithCI(entry->name(), ".pod") && PodArchive::isPodArchive(entry->data()))
		new_archive = new PodArchive();
	else if (ChasmBinArchive::isChasmBinArchive(entry->data()))
		new_archive = new ChasmBinArchive();
	else if (SiNArchive::isSiNArchive(entry->data()))
		new_archive = new SiNArchive();
	else
	{
		// Unsupported format
		Global::error = "Unsupported or invalid Archive format";
		return nullptr;
	}

	// If it opened successfully, add it to the list & return it,
	// Otherwise, delete it and return nullptr
	if (new_archive->open(entry))
	{
		if (manage)
		{
			// Add to parent's child list if parent is open in the manager (it should be)
			int index_parent = -1;
			if (entry->parent())
				index_parent = archiveIndex(entry->parent());
			if (index_parent >= 0)
				open_archives_[index_parent].open_children.push_back(new_archive);

			// Add the new archive
			addArchive(new_archive);

			// Announce open
			if (!silent)
			{
				MemChunk mc;
				uint32_t index = archiveIndex(new_archive);
				mc.write(&index, 4);
				announce("archive_opened", mc);
			}
		}

		return new_archive;
	}
	else
	{
		Log::error(Global::error);
		delete new_archive;
		return nullptr;
	}
}

// -----------------------------------------------------------------------------
// Opens [dir] as a DirArchive and adds it to the list.
// Returns a pointer to the archive or nullptr if an error occurred.
// -----------------------------------------------------------------------------
Archive* ArchiveManager::openDirArchive(const wxString& dir, bool manage, bool silent)
{
	auto new_archive = getArchive(dir);

	Log::info(wxString::Format("Opening directory %s as archive", dir));

	// If the archive is already open, just return it
	if (new_archive)
	{
		// Announce open
		if (!silent)
		{
			MemChunk mc;
			uint32_t index = archiveIndex(new_archive);
			mc.write(&index, 4);
			announce("archive_opened", mc);
		}

		return new_archive;
	}

	new_archive = new DirArchive();

	// If it opened successfully, add it to the list if needed & return it,
	// Otherwise, delete it and return nullptr
	if (new_archive->open(dir))
	{
		if (manage)
		{
			// Add the archive
			addArchive(new_archive);

			// Announce open
			if (!silent)
			{
				MemChunk mc;
				uint32_t index = archiveIndex(new_archive);
				mc.write(&index, 4);
				announce("archive_opened", mc);
			}

			// Add to recent files
			addRecentFile(dir);
		}

		// Return the opened archive
		return new_archive;
	}
	else
	{
		Log::error(Global::error);
		delete new_archive;
		return nullptr;
	}
}

// -----------------------------------------------------------------------------
// Creates a new archive of the specified format and adds it to the list of open
// archives. Returns the created archive, or nullptr if an invalid archive type
// was given
// -----------------------------------------------------------------------------
Archive* ArchiveManager::newArchive(const wxString& format)
{
	// Create a new archive depending on the type specified
	Archive* new_archive;
	if (format == "wad")
		new_archive = new WadArchive();
	else if (format == "zip")
		new_archive = new ZipArchive();
	else
	{
		Global::error = wxString::Format("Can not create archive of format: %s", CHR(format));
		Log::error(Global::error);
		return nullptr;
	}

	// If the archive was created, set its filename and add it to the list
	if (new_archive)
	{
		new_archive->setFilename(wxString::Format("UNSAVED (%s)", new_archive->formatDesc().name));
		addArchive(new_archive);
	}

	// Return the created archive, if any
	return new_archive;
}

// -----------------------------------------------------------------------------
// Closes the archive at index, and removes it from the list if the index is
// valid. Returns false on invalid index, true otherwise
// -----------------------------------------------------------------------------
bool ArchiveManager::closeArchive(int index)
{
	// Check for invalid index
	if (index < 0 || index >= (int)open_archives_.size())
		return false;

	// Announce archive closing
	MemChunk mc;
	int32_t  temp = index;
	mc.write(&temp, 4);
	announce("archive_closing", mc);

	// Delete any bookmarked entries contained in the archive
	deleteBookmarksInArchive(open_archives_[index].archive);

	// Remove from resource manager
	App::resources().removeArchive(open_archives_[index].archive);

	// Close any open child archives
	// Clear out the open_children vector first, lest the children try to remove themselves from it
	auto open_children = open_archives_[index].open_children;
	open_archives_[index].open_children.clear();
	for (auto& archive : open_children)
	{
		int ci = archiveIndex(archive);
		if (ci >= 0)
			closeArchive(ci);
	}

	// Remove ourselves from our parent's open-child list
	auto parent = open_archives_[index].archive->parentEntry();
	if (parent)
	{
		auto gp = parent->parent();
		if (gp)
		{
			int pi = archiveIndex(gp);
			if (pi >= 0)
			{
				auto& children = open_archives_[pi].open_children;
				for (auto it = children.begin(); it < children.end(); ++it)
				{
					if (*it == open_archives_[index].archive)
					{
						children.erase(it, it + 1);
						break;
					}
				}
			}
		}
	}

	// Close the archive
	open_archives_[index].archive->close();

	// Delete the archive object
	delete open_archives_[index].archive;

	// Remove the archive at index from the list
	open_archives_.erase(open_archives_.begin() + index);

	// Announce closed
	announce("archive_closed", mc);

	return true;
}

// -----------------------------------------------------------------------------
// Finds the archive with a matching filename, deletes it and removes it from
// the list.
// Returns false if it doesn't exist or can't be removed, true otherwise
// -----------------------------------------------------------------------------
bool ArchiveManager::closeArchive(const wxString& filename)
{
	// Go through all open archives
	for (int a = 0; a < (int)open_archives_.size(); a++)
	{
		// If the filename matches, remove it
		if (open_archives_[a].archive->filename().compare(filename) == 0)
			return closeArchive(a);
	}

	// If no archive is found with a matching filename, return false
	return false;
}

// -----------------------------------------------------------------------------
// Closes the specified archive and removes it from the list, if it exists in
// the list. Returns false if it doesn't exist, else true
// -----------------------------------------------------------------------------
bool ArchiveManager::closeArchive(Archive* archive)
{
	// Go through all open archives
	for (int a = 0; a < (int)open_archives_.size(); a++)
	{
		// If the archive exists in the list, remove it
		if (open_archives_[a].archive == archive)
			return closeArchive(a);
	}

	// If the archive isn't in the list, return false
	return false;
}

// -----------------------------------------------------------------------------
// Closes all opened archives
// -----------------------------------------------------------------------------
void ArchiveManager::closeAll()
{
	// Close the first archive in the list until no archives are open
	while (!open_archives_.empty())
		closeArchive(0);
}

// -----------------------------------------------------------------------------
// Returns the index in the list of the given archive, or -1 if the archive
// doesn't exist in the list
// -----------------------------------------------------------------------------
int ArchiveManager::archiveIndex(Archive* archive)
{
	// Go through all open archives
	for (size_t a = 0; a < open_archives_.size(); a++)
	{
		// If the archive we're looking for is this one, return the index
		if (open_archives_[a].archive == archive)
			return (int)a;
	}

	// If we get to here the archive wasn't found, so return -1
	return -1;
}

// -----------------------------------------------------------------------------
// Returns all open archives that live inside this one, recursively.
// -----------------------------------------------------------------------------
// This is the recursive bit, separate from the public entry point
void ArchiveManager::getDependentArchivesInternal(Archive* archive, vector<Archive*>& vec)
{
	int ai = archiveIndex(archive);

	for (auto& child : open_archives_[ai].open_children)
	{
		vec.push_back(child);

		getDependentArchivesInternal(child, vec);
	}
}
vector<Archive*> ArchiveManager::getDependentArchives(Archive* archive)
{
	vector<Archive*> vec;
	getDependentArchivesInternal(archive, vec);
	return vec;
}

// -----------------------------------------------------------------------------
// Returns a string containing the extensions of all supported archive formats,
// that can be used for wxWidgets file dialogs
// -----------------------------------------------------------------------------
wxString ArchiveManager::getArchiveExtensionsString() const
{
	auto             formats = Archive::allFormats();
	vector<wxString> ext_strings;
	wxString         ext_all = "Any supported file|";
	for (const auto& fmt : formats)
	{
		for (auto ext : fmt.extensions)
		{
			wxString ext_case = wxString::Format("*.%s;", ext.first.Lower());
			ext_case += wxString::Format("*.%s;", ext.first.Upper());
			ext_case += wxString::Format("*.%s", ext.first.Capitalize());

			ext_all += wxString::Format("%s;", ext_case);
			ext_strings.push_back(wxString::Format("%s files (*.%s)|%s", ext.second, ext.first, ext_case));
		}
	}

	ext_all.RemoveLast(1);
	for (const auto& ext : ext_strings)
		ext_all += wxString::Format("|%s", ext);

	return ext_all;
}

// -----------------------------------------------------------------------------
// Returns true if [archive] is set to be used as a resource, false otherwise
// -----------------------------------------------------------------------------
bool ArchiveManager::archiveIsResource(Archive* archive)
{
	int index = archiveIndex(archive);
	if (index < 0)
		return false;
	else
		return open_archives_[index].resource;
}

// -----------------------------------------------------------------------------
// Sets/unsets [archive] to be used as a resource
// -----------------------------------------------------------------------------
void ArchiveManager::setArchiveResource(Archive* archive, bool resource)
{
	int index = archiveIndex(archive);
	if (index >= 0)
	{
		bool was_resource              = open_archives_[index].resource;
		open_archives_[index].resource = resource;

		// Update resource manager
		if (resource && !was_resource)
			App::resources().addArchive(archive);
		else if (!resource && was_resource)
			App::resources().removeArchive(archive);
	}
}

// -----------------------------------------------------------------------------
// Adds [path] to the list of base resource paths
// -----------------------------------------------------------------------------
bool ArchiveManager::addBaseResourcePath(const wxString& path)
{
	// Firstly, check the file exists
	if (!wxFileExists(path))
		return false;

	// Second, check the path doesn't already exist
	for (auto& base_resource_path : base_resource_paths_)
	{
		if (S_CMP(base_resource_path, path))
			return false;
	}

	// Add it
	base_resource_paths_.push_back(path);

	// Announce
	announce("base_resource_path_added");

	return true;
}

// -----------------------------------------------------------------------------
// Removes the base resource path at [index]
// -----------------------------------------------------------------------------
void ArchiveManager::removeBaseResourcePath(unsigned index)
{
	// Check index
	if (index >= base_resource_paths_.size())
		return;

	// Unload base resource if removed is open
	if (index == (unsigned)base_resource)
		openBaseResource(-1);

	// Modify base_resource cvar if needed
	else if (base_resource > (signed)index)
		base_resource = base_resource - 1;

	// Remove the path
	base_resource_paths_.erase(base_resource_paths_.begin() + index);

	// Announce
	announce("base_resource_path_removed");
}

// -----------------------------------------------------------------------------
// Returns the base resource path at [index]
// -----------------------------------------------------------------------------
wxString ArchiveManager::getBaseResourcePath(unsigned index)
{
	// Check index
	if (index >= base_resource_paths_.size())
		return "INVALID INDEX";

	return base_resource_paths_[index];
}

// -----------------------------------------------------------------------------
// Opens the base resource archive [index]
// -----------------------------------------------------------------------------
bool ArchiveManager::openBaseResource(int index)
{
	// Check we're opening a different archive
	if (base_resource_archive_ && base_resource == index)
		return true;

	// Close/delete current base resource archive
	if (base_resource_archive_)
	{
		App::resources().removeArchive(base_resource_archive_.get());
		base_resource_archive_ = nullptr;
	}

	// Check index
	if (index < 0 || (unsigned)index >= base_resource_paths_.size())
	{
		base_resource = -1;
		announce("base_resource_changed");
		return false;
	}

	// Create archive based on file type
	wxString filename = base_resource_paths_[index];
	if (WadArchive::isWadArchive(filename))
		base_resource_archive_ = std::make_unique<WadArchive>();
	else if (ZipArchive::isZipArchive(filename))
		base_resource_archive_ = std::make_unique<ZipArchive>();
	else
		return false;

	// Attempt to open the file
	UI::showSplash(wxString::Format("Opening %s...", filename), true);
	if (base_resource_archive_->open(filename))
	{
		base_resource = index;
		UI::hideSplash();
		App::resources().addArchive(base_resource_archive_.get());
		announce("base_resource_changed");
		return true;
	}
	base_resource_archive_ = nullptr;
	UI::hideSplash();
	announce("base_resource_changed");
	return false;
}

// -----------------------------------------------------------------------------
// Returns the first entry matching [name] in the resource archives.
// Resource archives = open archives -> base resource archives.
// -----------------------------------------------------------------------------
ArchiveEntry* ArchiveManager::getResourceEntry(const wxString& name, Archive* ignore)
{
	// Go through all open archives
	for (auto& open_archive : open_archives_)
	{
		// If it isn't a resource archive, skip it
		if (!open_archive.resource)
			continue;

		// If it's being ignored, skip it
		if (open_archive.archive == ignore)
			continue;

		// Try to find the entry in the archive
		auto entry = open_archive.archive->entry(name);
		if (entry)
			return entry;
	}

	// If entry isn't found yet, search the base resource archive
	if (base_resource_archive_)
		return base_resource_archive_->entry(name);

	return nullptr;
}

// -----------------------------------------------------------------------------
// Searches for an entry matching [options] in the resource archives
// -----------------------------------------------------------------------------
ArchiveEntry* ArchiveManager::findResourceEntry(Archive::SearchOptions& options, Archive* ignore)
{
	// Go through all open archives
	for (auto& open_archive : open_archives_)
	{
		// If it isn't a resource archive, skip it
		if (!open_archive.resource)
			continue;

		// If it's being ignored, skip it
		if (open_archive.archive == ignore)
			continue;

		// Try to find the entry in the archive
		auto entry = open_archive.archive->findLast(options);
		if (entry)
			return entry;
	}

	// If entry isn't found yet, search the base resource archive
	if (base_resource_archive_)
		return base_resource_archive_->findLast(options);

	return nullptr;
}

// -----------------------------------------------------------------------------
// Searches for entries matching [options] in the resource archives
// -----------------------------------------------------------------------------
vector<ArchiveEntry*> ArchiveManager::findAllResourceEntries(Archive::SearchOptions& options, Archive* ignore)
{
	vector<ArchiveEntry*> ret;

	// Search the base resource archive first
	if (base_resource_archive_)
	{
		auto vec = base_resource_archive_->findAll(options);
		ret.insert(ret.end(), vec.begin(), vec.end());
	}

	// Go through all open archives
	for (auto& open_archive : open_archives_)
	{
		// If it isn't a resource archive, skip it
		if (!open_archive.resource)
			continue;

		// If it's being ignored, skip it
		if (open_archive.archive == ignore)
			continue;

		// Add matching entries from this archive
		auto vec = open_archive.archive->findAll(options);
		ret.insert(ret.end(), vec.begin(), vec.end());
	}

	return ret;
}

// -----------------------------------------------------------------------------
// Returns the recent file path at [index]
// -----------------------------------------------------------------------------
wxString ArchiveManager::recentFile(unsigned index)
{
	// Check index
	if (index >= recent_files_.size())
		return "";

	return recent_files_[index];
}

// -----------------------------------------------------------------------------
// Adds a recent file to the list, if it doesn't exist already
// -----------------------------------------------------------------------------
void ArchiveManager::addRecentFile(wxString path)
{
	// Check the path is valid
	if (!(wxFileName::FileExists(path) || wxDirExists(path)))
		return;

	// Replace \ with /
	path.Replace("\\", "/");

	// Check if the file is already in the list
	for (unsigned a = 0; a < recent_files_.size(); a++)
	{
		if (recent_files_[a] == path)
		{
			// Move this file to the top of the list
			recent_files_.erase(recent_files_.begin() + a);
			recent_files_.insert(recent_files_.begin(), path);

			// Announce
			announce("recent_files_changed");

			return;
		}
	}

	// Add the file to the top of the list
	recent_files_.insert(recent_files_.begin(), path);

	// Keep it trimmed
	while (recent_files_.size() > (unsigned)max_recent_files)
		recent_files_.pop_back();

	// Announce
	announce("recent_files_changed");
}

// -----------------------------------------------------------------------------
// Adds a list of recent file paths to the recent file list
// -----------------------------------------------------------------------------
void ArchiveManager::addRecentFiles(vector<wxString> paths)
{
	// Mute annoucements
	setMuted(true);

	// Clear existing list
	recent_files_.clear();

	// Add the files
	for (const auto& path : paths)
		addRecentFile(path);

	// Announce
	setMuted(false);
	announce("recent_files_changed");
}

// -----------------------------------------------------------------------------
// Removes the recent file matching [path]
// -----------------------------------------------------------------------------
void ArchiveManager::removeRecentFile(const wxString& path)
{
	for (unsigned a = 0; a < recent_files_.size(); a++)
	{
		if (recent_files_[a] == path)
		{
			recent_files_.erase(recent_files_.begin() + a);
			announce("recent_files_changed");
			return;
		}
	}
}

// -----------------------------------------------------------------------------
// Adds [entry] to the bookmark list
// -----------------------------------------------------------------------------
void ArchiveManager::addBookmark(ArchiveEntry* entry)
{
	// Check the bookmark isn't already in the list
	for (auto& bookmark : bookmarks_)
	{
		if (bookmark == entry)
			return;
	}

	// Add bookmark
	bookmarks_.push_back(entry);

	// Announce
	announce("bookmarks_changed");
}

// -----------------------------------------------------------------------------
// Removes [entry] from the bookmarks list
// -----------------------------------------------------------------------------
bool ArchiveManager::deleteBookmark(ArchiveEntry* entry)
{
	// Find bookmark to remove
	for (unsigned a = 0; a < bookmarks_.size(); a++)
	{
		if (bookmarks_[a] == entry)
		{
			// Remove it
			bookmarks_.erase(bookmarks_.begin() + a);

			// Announce
			announce("bookmarks_changed");

			return true;
		}
	}

	// Entry not in bookmarks list
	return false;
}

// -----------------------------------------------------------------------------
// Removes the bookmarked entry [index]
// -----------------------------------------------------------------------------
bool ArchiveManager::deleteBookmark(unsigned index)
{
	// Check index
	if (index >= bookmarks_.size())
		return false;

	// Remove bookmark
	bookmarks_.erase(bookmarks_.begin() + index);

	// Announce
	announce("bookmarks_changed");

	return true;
}

// -----------------------------------------------------------------------------
// Removes any bookmarked entries in [archive] from the list
// -----------------------------------------------------------------------------
bool ArchiveManager::deleteBookmarksInArchive(Archive* archive)
{
	// Go through bookmarks
	bool removed = false;
	for (unsigned a = 0; a < bookmarks_.size(); a++)
	{
		// Check bookmarked entry's parent archive
		if (bookmarks_[a]->parent() == archive)
		{
			bookmarks_.erase(bookmarks_.begin() + a);
			a--;
			removed = true;
		}
	}

	if (removed)
	{
		// Announce
		announce("bookmarks_changed");
		return true;
	}
	else
		return false;
}

// -----------------------------------------------------------------------------
// Removes any bookmarked entries in [node] from the list
// -----------------------------------------------------------------------------
bool ArchiveManager::deleteBookmarksInDir(ArchiveTreeNode* node)
{
	// Go through bookmarks
	auto archive = node->archive();
	bool removed = deleteBookmark(node->dirEntry());
	for (unsigned a = 0; a < bookmarks_.size(); ++a)
	{
		// Check bookmarked entry's parent archive
		if (bookmarks_[a]->parent() == archive)
		{
			// Now check if the bookmarked entry is within
			// the removed dir or one of its descendants
			auto anode  = bookmarks_[a]->parentDir();
			bool remove = false;
			while (anode != archive->rootDir() && !remove)
			{
				if (anode == node)
					remove = true;
				else
					anode = (ArchiveTreeNode*)anode->parent();
			}
			if (remove)
			{
				bookmarks_.erase(bookmarks_.begin() + a);
				--a;
				removed = true;
			}
		}
	}

	if (removed)
	{
		// Announce
		announce("bookmarks_changed");
		return true;
	}
	else
		return false;
}

// -----------------------------------------------------------------------------
// Returns the bookmarked entry at [index]
// -----------------------------------------------------------------------------
ArchiveEntry* ArchiveManager::getBookmark(unsigned index)
{
	// Check index
	if (index >= bookmarks_.size())
		return nullptr;

	return bookmarks_[index];
}

// -----------------------------------------------------------------------------
// Called when an announcement is recieved from one of the archives in the list
// -----------------------------------------------------------------------------
void ArchiveManager::onAnnouncement(Announcer* announcer, const wxString& event_name, MemChunk& event_data)
{
	// Reset event data for reading
	event_data.seek(0, SEEK_SET);

	// Check that the announcement came from an archive in the list
	int32_t index = archiveIndex((Archive*)announcer);
	if (index >= 0)
	{
		// If the archive was saved
		if (event_name == "saved")
		{
			MemChunk mc;
			mc.write(&index, 4);
			announce("archive_saved", mc);
		}

		// If the archive was modified
		if (event_name == "modified" || event_name == "entry_modified")
		{
			MemChunk mc;
			mc.write(&index, 4);
			announce("archive_modified", mc);
		}
	}
}


// -----------------------------------------------------------------------------
//
// Console Commands
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Lists the filenames of all open archives
// -----------------------------------------------------------------------------
CONSOLE_COMMAND(list_archives, 0, true)
{
	Log::info(wxString::Format("%d Open Archives:", App::archiveManager().numArchives()));

	for (int a = 0; a < App::archiveManager().numArchives(); a++)
	{
		auto archive = App::archiveManager().getArchive(a);
		Log::info(wxString::Format("%d: \"%s\"", a + 1, archive->filename()));
	}
}

// -----------------------------------------------------------------------------
// Attempts to open each given argument (filenames)
// -----------------------------------------------------------------------------
void c_open(const vector<wxString>& args)
{
	for (const auto& arg : args)
		App::archiveManager().openArchive(arg);
}
ConsoleCommand am_open("open", &c_open, 1, true); // Can't use the macro with this name

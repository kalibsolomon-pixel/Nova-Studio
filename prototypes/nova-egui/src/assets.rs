use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    path::{Path, PathBuf},
};
#[derive(Clone, Default)]
pub struct Index {
    pub files: Vec<PathBuf>,
    pub directories: BTreeMap<PathBuf, Vec<PathBuf>>,
}
impl Index {
    pub fn scan(root: &Path) -> Result<Self, String> {
        if !root.is_dir() {
            return Err("Project directory does not exist".into());
        }
        let mut index = Self::default();
        index.directories.entry(PathBuf::new()).or_default();
        for entry in walkdir::WalkDir::new(root)
            .follow_links(false)
            .max_depth(32)
        {
            let entry = entry.map_err(|e| e.to_string())?;
            if entry.file_type().is_symlink() {
                continue;
            }
            if entry.file_type().is_file()
                && entry
                    .path()
                    .extension()
                    .is_some_and(|x| x.eq_ignore_ascii_case("png"))
            {
                let relative = entry
                    .path()
                    .strip_prefix(root)
                    .map_err(|e| e.to_string())?
                    .to_path_buf();
                index.files.push(relative.clone());
                let mut child = relative;
                while let Some(parent) = child.parent() {
                    let parent = parent.to_path_buf();
                    // Only a newly discovered directory needs its ancestors registered.
                    let new_directory = !index.directories.contains_key(&parent);
                    index
                        .directories
                        .entry(parent.clone())
                        .or_default()
                        .push(child.clone());
                    if !new_directory || parent.as_os_str().is_empty() {
                        break;
                    }
                    child = parent;
                }
            }
        }
        index.files.sort();
        for entries in index.directories.values_mut() {
            entries.sort();
        }
        Ok(index)
    }
}
#[derive(Clone, Default, Serialize, Deserialize)]
pub struct Navigation {
    pub directory: PathBuf,
    pub selected: Option<PathBuf>,
    pub query: String,
    pub grid: bool,
    pub reverse: bool,
}
impl Navigation {
    pub fn entries(&self, index: &Index) -> Vec<PathBuf> {
        let query = self.query.to_lowercase();
        let mut result = if query.is_empty() {
            index
                .directories
                .get(&self.directory)
                .cloned()
                .unwrap_or_default()
        } else {
            index
                .files
                .iter()
                .filter(|p| {
                    p.starts_with(&self.directory)
                        && p.to_string_lossy().to_lowercase().contains(&query)
                })
                .cloned()
                .collect()
        };
        result.sort_by_key(|p| (!index.directories.contains_key(p), p.clone()));
        if self.reverse {
            result.reverse();
        }
        result
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn index_search_and_mode_persistence() {
        let t = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(t.path().join("assets/a")).unwrap();
        for p in [
            "assets/a/stone.png",
            "assets/a/Glass.PNG",
            "assets/a/no.txt",
        ] {
            std::fs::write(t.path().join(p), []).unwrap();
        }
        let index = Index::scan(t.path()).unwrap();
        assert_eq!(index.files.len(), 2);
        assert_eq!(
            index.directories[Path::new("")],
            vec![PathBuf::from("assets")]
        );
        let mut n = Navigation {
            directory: PathBuf::from("assets/a"),
            selected: Some(PathBuf::from("assets/a/stone.png")),
            query: "STONE".into(),
            ..Default::default()
        };
        let original = n.clone();
        assert_eq!(n.entries(&index), vec![PathBuf::from("assets/a/stone.png")]);
        n.grid = true;
        assert_eq!(n.selected, original.selected);
        assert_eq!(n.directory, original.directory);
        assert_eq!(n.query, original.query);
        assert!(Index::scan(&t.path().join("missing")).is_err());
    }
}

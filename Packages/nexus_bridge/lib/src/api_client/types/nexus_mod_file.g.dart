// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'nexus_mod_file.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

NexusModFile _$NexusModFileFromJson(Map<String, dynamic> json) => NexusModFile(
  files: (json['files'] as List<dynamic>)
      .map((e) => FileElement.fromJson(e as Map<String, dynamic>))
      .toList(),
  fileUpdates: (json['file_updates'] as List<dynamic>)
      .map((e) => FileUpdate.fromJson(e as Map<String, dynamic>))
      .toList(),
);

Map<String, dynamic> _$NexusModFileToJson(NexusModFile instance) =>
    <String, dynamic>{
      'files': instance.files,
      'file_updates': instance.fileUpdates,
    };

FileUpdate _$FileUpdateFromJson(Map<String, dynamic> json) => FileUpdate(
  oldFileId: (json['old_file_id'] as num).toInt(),
  newFileId: (json['new_file_id'] as num).toInt(),
  oldFileName: json['old_file_name'] as String,
  newFileName: json['new_file_name'] as String,
  uploadedTimestamp: (json['uploaded_timestamp'] as num).toInt(),
  uploadedTime: DateTime.parse(json['uploaded_time'] as String),
);

Map<String, dynamic> _$FileUpdateToJson(FileUpdate instance) =>
    <String, dynamic>{
      'old_file_id': instance.oldFileId,
      'new_file_id': instance.newFileId,
      'old_file_name': instance.oldFileName,
      'new_file_name': instance.newFileName,
      'uploaded_timestamp': instance.uploadedTimestamp,
      'uploaded_time': instance.uploadedTime.toIso8601String(),
    };

FileElement _$FileElementFromJson(Map<String, dynamic> json) => FileElement(
  id: (json['id'] as List<dynamic>).map((e) => (e as num).toInt()).toList(),
  uid: (json['uid'] as num).toInt(),
  fileId: (json['file_id'] as num).toInt(),
  name: json['name'] as String,
  version: json['version'] as String,
  categoryId: (json['category_id'] as num).toInt(),
  isPrimary: json['is_primary'] as bool,
  size: (json['size'] as num).toInt(),
  fileName: json['file_name'] as String,
  uploadedTimestamp: (json['uploaded_timestamp'] as num).toInt(),
  uploadedTime: DateTime.parse(json['uploaded_time'] as String),
  modVersion: json['mod_version'] as String,
  externalVirusScanUrl: json['external_virus_scan_url'] as String?,
  description: json['description'] as String,
  sizeKb: (json['size_kb'] as num).toInt(),
  sizeInBytes: (json['size_in_bytes'] as num?)?.toInt(),
  changelogHtml: json['changelog_html'] as String?,
  contentPreviewLink: json['content_preview_link'] as String,
  categoryName: $enumDecodeNullable(
    _$CategoryNameEnumMap,
    json['category_name'],
    unknownValue: CategoryName.MAIN,
  ),
);

Map<String, dynamic> _$FileElementToJson(FileElement instance) =>
    <String, dynamic>{
      'id': instance.id,
      'uid': instance.uid,
      'file_id': instance.fileId,
      'name': instance.name,
      'version': instance.version,
      'category_id': instance.categoryId,
      'category_name': _$CategoryNameEnumMap[instance.categoryName],
      'is_primary': instance.isPrimary,
      'size': instance.size,
      'file_name': instance.fileName,
      'uploaded_timestamp': instance.uploadedTimestamp,
      'uploaded_time': instance.uploadedTime.toIso8601String(),
      'mod_version': instance.modVersion,
      'external_virus_scan_url': instance.externalVirusScanUrl,
      'description': instance.description,
      'size_kb': instance.sizeKb,
      'size_in_bytes': instance.sizeInBytes,
      'changelog_html': instance.changelogHtml,
      'content_preview_link': instance.contentPreviewLink,
    };

const _$CategoryNameEnumMap = {
  CategoryName.ARCHIVED: 'ARCHIVED',
  CategoryName.MAIN: 'MAIN',
  CategoryName.OLD_VERSION: 'OLD_VERSION',
  CategoryName.OPTIONAL: 'OPTIONAL',
};

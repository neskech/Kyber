// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'nexus_mod.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

NexusMod _$NexusModFromJson(Map<String, dynamic> json) => NexusMod(
  name: json['name'] as String,
  summary: json['summary'] as String,
  description: json['description'] as String,
  pictureUrl: json['picture_url'] as String,
  modDownloads: (json['mod_downloads'] as num).toInt(),
  modUniqueDownloads: (json['mod_unique_downloads'] as num).toInt(),
  uid: (json['uid'] as num).toInt(),
  modId: (json['mod_id'] as num).toInt(),
  gameId: (json['game_id'] as num).toInt(),
  allowRating: json['allow_rating'] as bool,
  domainName: json['domain_name'] as String,
  categoryId: (json['category_id'] as num).toInt(),
  version: json['version'] as String,
  endorsementCount: (json['endorsement_count'] as num).toInt(),
  createdTimestamp: (json['created_timestamp'] as num).toInt(),
  createdTime: DateTime.parse(json['created_time'] as String),
  updatedTimestamp: (json['updated_timestamp'] as num).toInt(),
  updatedTime: DateTime.parse(json['updated_time'] as String),
  author: json['author'] as String,
  uploadedBy: json['uploaded_by'] as String,
  uploadedUsersProfileUrl: json['uploaded_users_profile_url'] as String,
  containsAdultContent: json['contains_adult_content'] as bool,
  status: json['status'] as String,
  available: json['available'] as bool,
  user: User.fromJson(json['user'] as Map<String, dynamic>),
  endorsement: Endorsement.fromJson(
    json['endorsement'] as Map<String, dynamic>,
  ),
);

Map<String, dynamic> _$NexusModToJson(NexusMod instance) => <String, dynamic>{
  'name': instance.name,
  'summary': instance.summary,
  'description': instance.description,
  'picture_url': instance.pictureUrl,
  'mod_downloads': instance.modDownloads,
  'mod_unique_downloads': instance.modUniqueDownloads,
  'uid': instance.uid,
  'mod_id': instance.modId,
  'game_id': instance.gameId,
  'allow_rating': instance.allowRating,
  'domain_name': instance.domainName,
  'category_id': instance.categoryId,
  'version': instance.version,
  'endorsement_count': instance.endorsementCount,
  'created_timestamp': instance.createdTimestamp,
  'created_time': instance.createdTime.toIso8601String(),
  'updated_timestamp': instance.updatedTimestamp,
  'updated_time': instance.updatedTime.toIso8601String(),
  'author': instance.author,
  'uploaded_by': instance.uploadedBy,
  'uploaded_users_profile_url': instance.uploadedUsersProfileUrl,
  'contains_adult_content': instance.containsAdultContent,
  'status': instance.status,
  'available': instance.available,
  'user': instance.user,
  'endorsement': instance.endorsement,
};

Endorsement _$EndorsementFromJson(Map<String, dynamic> json) => Endorsement(
  endorseStatus: json['endorse_status'] as String,
  timestamp: json['timestamp'],
  version: json['version'],
);

Map<String, dynamic> _$EndorsementToJson(Endorsement instance) =>
    <String, dynamic>{
      'endorse_status': instance.endorseStatus,
      'timestamp': instance.timestamp,
      'version': instance.version,
    };

User _$UserFromJson(Map<String, dynamic> json) => User(
  memberId: (json['member_id'] as num).toInt(),
  memberGroupId: (json['member_group_id'] as num).toInt(),
  name: json['name'] as String,
);

Map<String, dynamic> _$UserToJson(User instance) => <String, dynamic>{
  'member_id': instance.memberId,
  'member_group_id': instance.memberGroupId,
  'name': instance.name,
};

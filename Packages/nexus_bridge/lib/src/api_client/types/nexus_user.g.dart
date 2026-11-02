// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'nexus_user.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

NexusUser _$NexusUserFromJson(Map<String, dynamic> json) => NexusUser(
  userId: (json['user_id'] as num?)?.toInt(),
  key: json['key'] as String?,
  name: json['name'] as String?,
  nexusUserIsPremium: json['is_premium?'] as bool?,
  nexusUserIsSupporter: json['is_supporter?'] as bool?,
  email: json['email'] as String?,
  profileUrl: json['profile_url'] as String?,
  isSupporter: json['is_supporter'] as bool?,
  isPremium: json['is_premium'] as bool?,
);

Map<String, dynamic> _$NexusUserToJson(NexusUser instance) => <String, dynamic>{
  'user_id': instance.userId,
  'key': instance.key,
  'name': instance.name,
  'is_premium?': instance.nexusUserIsPremium,
  'is_supporter?': instance.nexusUserIsSupporter,
  'email': instance.email,
  'profile_url': instance.profileUrl,
  'is_supporter': instance.isSupporter,
  'is_premium': instance.isPremium,
};
